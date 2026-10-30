#include "groove/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>

namespace groove {

static_assert(std::atomic<double>::is_always_lock_free, "render thread needs lock-free atomic<double>");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "render thread needs lock-free atomic<uint64_t>");
static_assert(std::atomic<bool>::is_always_lock_free, "render thread needs lock-free atomic<bool>");

namespace {

/// How many source bits a device sample format carries without rounding.
uint16_t exactBits(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::Int16: return 16;
        case SampleFormat::Int24Packed: return 24;
        case SampleFormat::Int32: return 32;
        case SampleFormat::Float32: return 24;  // 24-bit mantissa: every ≤24-bit integer sample is exact
    }
    return 16;
}

Error internalError(const char* where, const char* what) {
    return Error{ErrorCode::Internal, std::string(where) + ": " + (what ? what : "unknown exception")};
}

}  // namespace

// ============================================================================ construction

AudioEngine::AudioEngine(PrivateTag, EngineConfig config, std::unique_ptr<AudioOutput> output, std::shared_ptr<SourceOpener> opener)
    : cfg_(config), output_(std::move(output)), opener_(std::move(opener)) {}

Result<std::unique_ptr<AudioEngine>> AudioEngine::create(EngineConfig config, std::unique_ptr<AudioOutput> output,
                                                         std::shared_ptr<SourceOpener> opener) noexcept {
    using R = Result<std::unique_ptr<AudioEngine>>;
    if (!output) return R::fail(ErrorCode::InvalidArgument, "no audio output");
    config.ringSeconds = std::clamp(config.ringSeconds, 0.25, 10.0);
    config.prefillSeconds = std::clamp(config.prefillSeconds, 0.0, config.ringSeconds * 0.8);
    config.decodeChunkFrames = std::clamp<uint32_t>(config.decodeChunkFrames, 256, 65536);
    config.maxRenderFrames = std::clamp<uint32_t>(config.maxRenderFrames, 256, 65536);
    try {
        if (!opener) opener = std::make_shared<PathOpener>();
        auto e = std::make_unique<AudioEngine>(PrivateTag{}, config, std::move(output), std::move(opener));
        e->decodeBuf_.assign(static_cast<size_t>(config.decodeChunkFrames) * 8, 0);  // 8 = max channels
        e->markers_ = std::make_unique<SpscRing<Marker>>(32);
        e->decoderThread_ = std::thread(&AudioEngine::decoderLoop, e.get());
        try {
            e->controlThread_ = std::thread(&AudioEngine::controlLoop, e.get());
        } catch (...) {
            {
                std::lock_guard<std::mutex> lk(e->decMutex_);
                e->decQuit_ = true;
            }
            e->decCv_.notify_all();
            e->decoderThread_.join();
            throw;
        }
        return R(std::move(e));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "creating engine");
    } catch (const std::exception& ex) {
        return R::fail(ErrorCode::Internal, std::string("creating engine: ") + ex.what());
    } catch (...) {
        return R::fail(ErrorCode::Internal, "creating engine");
    }
}

AudioEngine::~AudioEngine() {
    post(CmdShutdown{});
    if (controlThread_.joinable()) controlThread_.join();
    // The control thread joins the decoder on shutdown; this covers the case where it never ran.
    if (decoderThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            decQuit_ = true;
        }
        decCv_.notify_all();
        decoderThread_.join();
    }
}

// ============================================================================ public commands

void AudioEngine::post(Command c) noexcept {
    try {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.push_back(std::move(c));
        ++posted_;
    } catch (...) {
        // Out of memory while queueing: the command is dropped, never half-applied.
        return;
    }
    qCv_.notify_one();
}

void AudioEngine::load(TrackSource track, LoadOptions opts) noexcept { post(CmdLoad{std::move(track), opts}); }
void AudioEngine::setNext(std::optional<TrackSource> track) noexcept { post(CmdSetNext{std::move(track)}); }
void AudioEngine::play() noexcept { post(CmdPlay{}); }
void AudioEngine::pause() noexcept { post(CmdPause{}); }
void AudioEngine::stop() noexcept { post(CmdStop{}); }
void AudioEngine::seek(double seconds) noexcept { post(CmdSeek{seconds}); }
void AudioEngine::setEq(const EqSettings& eq) noexcept { post(CmdEq{eq}); }
void AudioEngine::setVolume(double linear) noexcept { post(CmdVolume{linear}); }

void AudioEngine::setListener(std::weak_ptr<EngineListener> listener) noexcept {
    std::lock_guard<std::mutex> lk(infoMutex_);
    listener_ = std::move(listener);
}

bool AudioEngine::waitIdle(std::chrono::milliseconds timeout) noexcept {
    try {
        std::unique_lock<std::mutex> lk(qMutex_);
        const uint64_t target = posted_;
        return idleCv_.wait_for(lk, timeout, [&] { return processed_ >= target; });
    } catch (...) {
        return false;
    }
}

EngineSnapshot AudioEngine::snapshot() const noexcept {
    EngineSnapshot s;
    s.state = state_.load();
    s.trackId = heardTrack_.load();
    s.dspActive = dspActive_.load();
    s.underruns = underruns_.load();
    s.clippedSamples = clipped_.load();
    try {
        std::lock_guard<std::mutex> lk(infoMutex_);
        s.source = infoSource_;
        s.output = infoOutput_;
        s.bitPerfect = infoBitPerfect_;
        s.eqHeadroomDb = infoHeadroomDb_;
        s.lastError = lastError_.code;
        s.lastErrorMessage = lastError_.message;
    } catch (...) {
        // string copy failed (OOM): leave the message empty
    }
    if (s.source.sampleRate > 0 && s.trackId != 0) {
        s.positionSeconds = static_cast<double>(trackFrames_.load()) / s.source.sampleRate;
        s.durationSeconds = static_cast<double>(heardTotalFrames_.load()) / s.source.sampleRate;
        if (s.durationSeconds > 0) s.positionSeconds = std::min(s.positionSeconds, s.durationSeconds);
    }
    return s;
}

bool AudioEngine::isTransitionAllowed(PlaybackState from, PlaybackState to) noexcept {
    using S = PlaybackState;
    if (from == to) return true;
    switch (from) {
        case S::Stopped: return to == S::Loading;
        case S::Loading: return to == S::Playing || to == S::Paused || to == S::Error || to == S::Stopped;
        case S::Playing: return to == S::Paused || to == S::Stopped || to == S::Loading || to == S::Error;
        case S::Paused: return to == S::Playing || to == S::Stopped || to == S::Loading || to == S::Error;
        case S::Error: return to == S::Loading || to == S::Stopped;
    }
    return false;
}

size_t AudioEngine::readTap(float* dst, size_t maxSamples) noexcept {
    std::lock_guard<std::mutex> lk(tapMutex_);
    if (!tap_ || !dst) return 0;
    const size_t ch = outFormat_.channels ? outFormat_.channels : 1;
    const size_t avail = tap_->readable();
    const size_t want = maxSamples / ch * ch;
    if (avail > want) tap_->skip((avail - want) / ch * ch);  // keep only the newest audio
    return tap_->read(dst, want);
}

// ============================================================================ control thread

void AudioEngine::controlLoop() noexcept {
    for (;;) {
        std::optional<Command> cmd;
        try {
            std::unique_lock<std::mutex> lk(qMutex_);
            // Wake at least every 10 ms to react to flags raised by the render thread (it never blocks or signals).
            qCv_.wait_for(lk, std::chrono::milliseconds(10), [&] { return !queue_.empty(); });
            if (!queue_.empty()) {
                cmd.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }
        } catch (...) {
            // std::system_error from the mutex: nothing sensible to do but retry
        }
        const bool shutdown = cmd && std::holds_alternative<CmdShutdown>(*cmd);
        if (cmd) {
            try {
                handle(*cmd);
            } catch (const std::bad_alloc&) {
                setError(Error{ErrorCode::OutOfMemory, "out of memory"});
                stopTheWorld();
                transitionTo(PlaybackState::Error);
            } catch (const std::exception& ex) {
                setError(internalError("command", ex.what()));
                stopTheWorld();
                transitionTo(PlaybackState::Error);
            } catch (...) {
                setError(internalError("command", nullptr));
                stopTheWorld();
                transitionTo(PlaybackState::Error);
            }
            {
                std::lock_guard<std::mutex> lk(qMutex_);
                ++processed_;
            }
            idleCv_.notify_all();
        }
        if (shutdown) break;
        try {
            pollRealtimeFlags();
        } catch (...) {
            setError(internalError("poll", nullptr));
            stopTheWorld();
            transitionTo(PlaybackState::Error);
        }
    }
    // Teardown: output first (so the render callback stops), then the decoder thread.
    stopTheWorld();
    if (outputOpen_) output_->close();
    outputOpen_ = false;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        decQuit_ = true;
    }
    decCv_.notify_all();
    if (decoderThread_.joinable()) decoderThread_.join();
}

void AudioEngine::handle(Command& c) {
    if (auto* x = std::get_if<CmdLoad>(&c)) return onLoad(*x);
    if (auto* x = std::get_if<CmdSetNext>(&c)) return onSetNext(*x);
    if (std::holds_alternative<CmdPlay>(c)) return onPlay();
    if (std::holds_alternative<CmdPause>(c)) return onPause();
    if (std::holds_alternative<CmdStop>(c)) return onStop();
    if (auto* x = std::get_if<CmdSeek>(&c)) return onSeek(x->seconds);
    if (auto* x = std::get_if<CmdEq>(&c)) return onEq(x->eq);
    if (auto* x = std::get_if<CmdVolume>(&c)) return onVolume(x->v);
    if (auto* x = std::get_if<CmdDecodeFailed>(&c)) return onDecodeFailed(*x);
    // CmdShutdown: handled by the loop
}

bool AudioEngine::transitionTo(PlaybackState s) noexcept {
    const PlaybackState from = state_.load();
    if (from == s) return true;
    if (!isTransitionAllowed(from, s)) return false;  // ignored, never thrown
    state_.store(s);
    const uint64_t id = heardTrack_.load();
    notify([&](EngineListener& l) { l.onStateChanged(s, id); });
    return true;
}

void AudioEngine::setError(const Error& err) noexcept {
    try {
        std::lock_guard<std::mutex> lk(infoMutex_);
        lastError_ = err;
    } catch (...) {
    }
}

template <typename F>
void AudioEngine::notify(F&& f) noexcept {
    std::shared_ptr<EngineListener> l;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        l = listener_.lock();
    }
    if (l) f(*l);  // listener methods are noexcept
}

void AudioEngine::rememberSource(const TrackSource& t) {
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const TrackSource& s) { return s.id == t.id; }), recent_.end());
    recent_.push_back(t);
    while (recent_.size() > 6) recent_.pop_front();
}

Result<AudioEngine::Slot> AudioEngine::openSlot(const TrackSource& t) noexcept {
    using R = Result<Slot>;
    try {
        auto src = opener_->open(t.uri);
        if (!src) return src.error();
        auto dec = openDecoder(std::move(src).value());
        if (!dec) return dec.error();
        if (!dec.value()->format().valid()) return R::fail(ErrorCode::Unsupported, t.uri + ": unsupported stream format");
        Slot s;
        s.decoder = std::move(dec).value();
        s.source = t;
        return R(std::move(s));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, t.uri);
    } catch (...) {
        return R::fail(ErrorCode::Internal, t.uri + ": exception while opening");
    }
}

bool AudioEngine::needsReopen(const StreamFormat& f) const noexcept {
    return !outputOpen_ || !f.gaplessCompatible(outFormat_) || f.bitsPerSample > outHoldsBits_;
}

void AudioEngine::onLoad(CmdLoad& c) {
    stopTheWorld();
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        current_ = Slot{};
        finished_ = Slot{};
        next_ = Slot{};  // a new load starts a new queue; the app sets the next track again
    }
    transitionTo(PlaybackState::Loading);
    auto slot = openSlot(c.track);
    if (!slot) {
        notify([&](EngineListener& l) { l.onTrackFailed(c.track.id, slot.error().code, slot.error().message); });
        setError(slot.error());
        transitionTo(PlaybackState::Error);
        return;
    }
    rememberSource(c.track);
    Slot s = std::move(slot).value();
    uint64_t startFrame = 0;
    if (c.opts.startSeconds > 0) {
        const uint64_t total = s.decoder->totalFrames();
        startFrame = static_cast<uint64_t>(c.opts.startSeconds * s.decoder->format().sampleRate);
        if (total > 0 && startFrame >= total) startFrame = total - 1;
        if (!s.decoder->seek(startFrame)) startFrame = 0;  // fall back to the start rather than fail
    }
    startSlot(std::move(s), startFrame, c.opts.autoplay);
}

void AudioEngine::startSlot(Slot slot, uint64_t startFrame, bool autoplay) {
    transitionTo(PlaybackState::Loading);
    const StreamFormat fmt = slot.decoder->format();
    const uint64_t id = slot.source.id;
    if (needsReopen(fmt)) {
        auto r = configureOutput(fmt);
        if (!r) {
            notify([&](EngineListener& l) { l.onTrackFailed(id, r.error().code, r.error().message); });
            setError(r.error());
            transitionTo(PlaybackState::Error);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        current_ = std::move(slot);
    }
    resetPipeline(current_, startFrame);
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        infoSource_ = fmt;
    }
    updateBitPerfect();
    notify([&](EngineListener& l) { l.onTrackStarted(id); });
    runDecoder();
    if (autoplay) {
        auto r = startOutputAfterPrefill();
        if (!r) {
            setError(r.error());
            stopTheWorld();
            transitionTo(PlaybackState::Error);
            return;
        }
        transitionTo(PlaybackState::Playing);
    } else {
        transitionTo(PlaybackState::Paused);
    }
}

void AudioEngine::onSetNext(CmdSetNext& c) {
    if (!c.track) {
        std::lock_guard<std::mutex> lk(decMutex_);
        next_ = Slot{};
        return;
    }
    // Ahead-of-time load: the file is opened and its headers parsed now, long before it's needed,
    // so the end-of-track swap on the decoder thread is just a pointer move.
    auto slot = openSlot(*c.track);
    if (!slot) {
        notify([&](EngineListener& l) { l.onTrackFailed(c.track->id, slot.error().code, slot.error().message); });
        return;
    }
    rememberSource(*c.track);
    std::lock_guard<std::mutex> lk(decMutex_);
    next_ = std::move(slot).value();
}

void AudioEngine::onPlay() {
    if (state_.load() != PlaybackState::Paused) return;
    if (!outputRunning_) {
        auto r = startOutputAfterPrefill();
        if (!r) {
            setError(r.error());
            if (!recoverDevice()) {
                stopTheWorld();
                transitionTo(PlaybackState::Error);
                return;
            }
        }
    }
    transitionTo(PlaybackState::Playing);
}

void AudioEngine::onPause() {
    if (state_.load() != PlaybackState::Playing) return;
    if (outputRunning_) {
        (void)output_->stop();  // synchronous: the render callback has returned and won't be called again
        outputRunning_ = false;
    }
    transitionTo(PlaybackState::Paused);  // the decoder keeps the ring full while paused
}

void AudioEngine::onStop() {
    stopTheWorld();
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        current_ = Slot{};
        finished_ = Slot{};
        next_ = Slot{};
    }
    heardTrack_.store(0);
    trackFrames_.store(0);
    heardTotalFrames_.store(0);
    transitionTo(PlaybackState::Stopped);
}

void AudioEngine::onSeek(double seconds) {
    const PlaybackState before = state_.load();
    if (before != PlaybackState::Playing && before != PlaybackState::Paused) return;
    stopTheWorld();
    const uint64_t heard = heardTrack_.load();
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        if (current_ && current_.source.id == heard) {
            found = true;
        } else if (finished_ && finished_.source.id == heard) {
            // We're hearing the tail of a track the decoder already finished; it has moved on to the next
            // one. Put that one back in the "next" slot and continue in the heard track.
            if (current_ && !next_) {
                next_ = std::move(current_);
                (void)next_.decoder->seek(0);
            }
            current_ = std::move(finished_);
            found = true;
        }
    }
    if (!found) {
        // Neither decoder is around any more (very short tracks): reopen the heard track from its source.
        auto it = std::find_if(recent_.begin(), recent_.end(), [&](const TrackSource& s) { return s.id == heard; });
        if (it == recent_.end()) return;
        auto slot = openSlot(*it);
        if (!slot) return failAndMaybeSkip(heard, slot.error());
        std::lock_guard<std::mutex> lk(decMutex_);
        if (current_ && !next_) next_ = std::move(current_);
        current_ = std::move(slot).value();
    }
    const StreamFormat fmt = current_.decoder->format();
    const uint64_t total = current_.decoder->totalFrames();
    uint64_t frame = static_cast<uint64_t>(std::max(0.0, seconds) * fmt.sampleRate);
    if (total > 0 && frame >= total) frame = total - 1;
    auto r = current_.decoder->seek(frame);
    if (!r) return failAndMaybeSkip(current_.source.id, r.error());
    if (needsReopen(fmt)) {
        auto cr = configureOutput(fmt);
        if (!cr) return failAndMaybeSkip(current_.source.id, cr.error());
    }
    resetPipeline(current_, frame);
    runDecoder();
    if (before == PlaybackState::Playing) {
        auto sr = startOutputAfterPrefill();
        if (!sr) {
            setError(sr.error());
            stopTheWorld();
            transitionTo(PlaybackState::Error);
        }
    }
}

void AudioEngine::onEq(const EqSettings& eq) {
    eqSettings_ = eq;
    const double rate = outFormat_.sampleRate ? outFormat_.sampleRate : 48000.0;
    const EqDesign d = designEq(eq, rate);
    eqDesigns_.publish(d);
    dspActive_.store(!d.transparent || volumeValue_ != 1.0);
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        infoHeadroomDb_ = d.headroomDb;
    }
    updateBitPerfect();
}

void AudioEngine::onVolume(double v) {
    if (!std::isfinite(v)) return;
    volumeValue_ = std::clamp(v, 0.0, 1.0);  // no boost: use the EQ preamp for that
    volume_.store(volumeValue_);
    const bool eqTransparent = designEq(eqSettings_, outFormat_.sampleRate ? outFormat_.sampleRate : 48000.0).transparent;
    dspActive_.store(!eqTransparent || volumeValue_ != 1.0);
    updateBitPerfect();
}

void AudioEngine::onDecodeFailed(const CmdDecodeFailed& c) {
    failAndMaybeSkip(c.trackId, c.error);
}

void AudioEngine::failAndMaybeSkip(uint64_t trackId, const Error& err) {
    notify([&](EngineListener& l) { l.onTrackFailed(trackId, err.code, err.message); });
    setError(err);
    const bool wasPlaying = state_.load() == PlaybackState::Playing;
    stopTheWorld();
    Slot fallback;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        fallback = std::move(next_);
        current_ = Slot{};
        finished_ = Slot{};
    }
    if (fallback) {
        // Safe recovery: the drive or file went away, carry on with the next track in the queue.
        startSlot(std::move(fallback), 0, wasPlaying);
    } else {
        transitionTo(PlaybackState::Error);
    }
}

void AudioEngine::pollRealtimeFlags() {
    if (flagTrackChanged_.exchange(false)) {
        const uint64_t id = heardTrack_.load();
        StreamFormat fmt{};
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            if (current_ && current_.source.id == id) fmt = current_.decoder->format();
            else if (finished_ && finished_.source.id == id) fmt = finished_.decoder->format();
            // The previous track is no longer audible: release its decoder (and file) now.
            if (finished_ && finished_.source.id != id) finished_ = Slot{};
        }
        if (fmt.valid()) {
            std::lock_guard<std::mutex> lk(infoMutex_);
            infoSource_ = fmt;
        }
        updateBitPerfect();
        notify([&](EngineListener& l) { l.onTrackStarted(id); });
    }
    if (flagReconfigure_.exchange(false)) {
        // The render thread reached a track whose format needs a different output stream.
        const bool playing = state_.load() == PlaybackState::Playing;
        stopTheWorld();
        if (!current_) return;
        const StreamFormat fmt = current_.decoder->format();
        // The render thread raises "track changed" together with "reconfigure"; if we haven't reported it
        // yet, do it here, because resetPipeline() clears the flag.
        const bool pendingStart = flagTrackChanged_.exchange(false);
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            finished_ = Slot{};
        }
        auto r = configureOutput(fmt);
        if (!r) return failAndMaybeSkip(current_.source.id, r.error());
        resetPipeline(current_, 0);
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            infoSource_ = fmt;
        }
        updateBitPerfect();
        if (pendingStart) {
            const uint64_t id = current_.source.id;
            notify([&](EngineListener& l) { l.onTrackStarted(id); });
        }
        runDecoder();
        if (playing) {
            auto sr = startOutputAfterPrefill();
            if (!sr) {
                setError(sr.error());
                stopTheWorld();
                transitionTo(PlaybackState::Error);
            }
        }
    }
    if (flagEnded_.exchange(false)) {
        stopTheWorld();
        Slot late;
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            current_ = Slot{};
            finished_ = Slot{};
            late = std::move(next_);  // setNext() arrived after the decoder had already finished
        }
        if (late) {
            startSlot(std::move(late), 0, true);
        } else {
            heardTrack_.store(0);
            trackFrames_.store(0);
            transitionTo(PlaybackState::Stopped);
            notify([](EngineListener& l) { l.onQueueEnded(); });
        }
    }
    if (flagDeviceError_.exchange(false)) {
        if (!recoverDevice()) {
            setError(Error{ErrorCode::DeviceLost, "audio output lost and could not be reopened"});
            stopTheWorld();
            transitionTo(PlaybackState::Error);
        }
    }
}

// ============================================================================ pipeline management

void AudioEngine::stopTheWorld() noexcept {
    if (outputRunning_) {
        (void)output_->stop();
        outputRunning_ = false;
    }
    parkDecoder();
}

void AudioEngine::parkDecoder() noexcept {
    try {
        std::unique_lock<std::mutex> lk(decMutex_);
        decRun_ = false;
        decCv_.notify_all();
        decIdleCv_.wait(lk, [&] { return !decBusy_; });
    } catch (...) {
    }
}

void AudioEngine::runDecoder() noexcept {
    try {
        std::lock_guard<std::mutex> lk(decMutex_);
        decRun_ = true;
    } catch (...) {
    }
    decCv_.notify_all();
}

void AudioEngine::resetPipeline(const Slot& heard, uint64_t startFrame) noexcept {
    // Precondition: output stopped and decoder parked, so nobody else touches the rings.
    if (ring_) ring_->reset();
    markers_->reset();
    rtHasMarker_ = false;
    rtStopped_.store(false);
    flagTrackChanged_.store(false);
    flagReconfigure_.store(false);
    flagEnded_.store(false);
    rtEq_.reset();
    heardTrack_.store(heard.source.id);
    trackFrames_.store(startFrame);
    heardTotalFrames_.store(heard.decoder ? heard.decoder->totalFrames() : 0);
    try {
        std::lock_guard<std::mutex> lk(decMutex_);
        decStatus_ = DecodeStatus::Running;
        decAtEnd_ = false;
    } catch (...) {
    }
}

Result<void> AudioEngine::configureOutput(const StreamFormat& fmt) noexcept {
    try {
        if (outputOpen_) {
            if (outputRunning_) (void)output_->stop();
            output_->close();
            outputOpen_ = false;
            outputRunning_ = false;
        }
        OutputRequest req;
        req.sampleRate = fmt.sampleRate;
        req.channels = fmt.channels;
        req.sourceBits = fmt.bitsPerSample;
        auto r = output_->open(req, this);
        if (!r) return Result<void>::fail(ErrorCode::DeviceOpenFailed, r.error().message);
        const OutputConfig oc = r.value();
        if (oc.sampleRate != fmt.sampleRate || oc.channels != fmt.channels) {
            // The core never resamples or remixes; the platform output must accept the source layout
            // (and let the OS convert if it has to, reporting osResamples).
            output_->close();
            return Result<void>::fail(ErrorCode::DeviceOpenFailed, "output did not accept the source rate/channels");
        }
        outConfig_ = oc;
        outFormat_ = fmt;
        outHoldsBits_ = exactBits(oc.format);
        outputOpen_ = true;

        const size_t ch = fmt.channels;
        const size_t want = std::max<size_t>(static_cast<size_t>(cfg_.ringSeconds * fmt.sampleRate) * ch, cfg_.decodeChunkFrames * ch * 4);
        if (!ring_ || ring_->capacity() < want || ring_->capacity() > want * 4) ring_ = std::make_unique<SpscRing<int32_t>>(want);
        rtScratch_.assign(static_cast<size_t>(cfg_.maxRenderFrames) * ch, 0);
        rtDsp_.assign(static_cast<size_t>(cfg_.maxRenderFrames) * ch, 0.0);
        rtEq_.prepare(fmt.channels);
        rtTapBuf_.assign(static_cast<size_t>(cfg_.maxRenderFrames) * ch, 0.0f);
        {
            std::lock_guard<std::mutex> lk(tapMutex_);
            tap_ = std::make_unique<SpscRing<float>>(static_cast<size_t>(fmt.sampleRate / 4) * ch);  // 250 ms
        }
        const EqDesign d = designEq(eqSettings_, fmt.sampleRate);
        rtDesign_ = d;           // render thread is stopped: direct write is safe
        eqDesigns_.publish(d);
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            infoHeadroomDb_ = d.headroomDb;
        }
        dspActive_.store(!d.transparent || volumeValue_ != 1.0);
        updateBitPerfect();
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::fail(ErrorCode::OutOfMemory, "allocating audio buffers");
    } catch (...) {
        return Result<void>::fail(ErrorCode::Internal, "configuring output");
    }
}

Result<void> AudioEngine::startOutputAfterPrefill() noexcept {
    if (!outputOpen_ || !ring_) return Result<void>::fail(ErrorCode::InvalidState, "output not open");
    const size_t want = std::min(static_cast<size_t>(cfg_.prefillSeconds * outFormat_.sampleRate) * outFormat_.channels,
                                 ring_->capacity() * 9 / 10);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ring_->readable() < want && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            if (decStatus_ != DecodeStatus::Running || decAtEnd_) break;  // track fully decoded, or a failure
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto r = output_->start();
    if (!r) return r;
    outputRunning_ = true;
    return Result<void>::success();
}

bool AudioEngine::recoverDevice() noexcept {
    const bool wasRunning = outputRunning_ || state_.load() == PlaybackState::Playing;
    for (int attempt = 0; attempt < std::max(1, cfg_.deviceReopenAttempts); ++attempt) {
        if (outputRunning_) (void)output_->stop();
        if (outputOpen_) output_->close();
        outputRunning_ = false;
        outputOpen_ = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << (2 * attempt))));  // 100, 400, 1600 ms
        OutputRequest req;
        req.sampleRate = outFormat_.sampleRate;
        req.channels = outFormat_.channels;
        req.sourceBits = outFormat_.bitsPerSample;
        auto r = output_->open(req, this);
        if (!r) continue;
        if (r.value().sampleRate != outFormat_.sampleRate || r.value().channels != outFormat_.channels) {
            output_->close();
            continue;
        }
        outConfig_ = r.value();
        outHoldsBits_ = exactBits(outConfig_.format);
        outputOpen_ = true;
        updateBitPerfect();
        if (wasRunning) {
            if (!output_->start()) continue;
            outputRunning_ = true;
        }
        return true;
    }
    return false;
}

void AudioEngine::updateBitPerfect() noexcept {
    try {
        std::lock_guard<std::mutex> lk(infoMutex_);
        const StreamFormat& src = infoSource_;
        const bool bp = outputOpen_ && src.valid() && !src.fromDsd && !outConfig_.osResamples &&
                        outConfig_.sampleRate == src.sampleRate && outConfig_.channels == src.channels &&
                        exactBits(outConfig_.format) >= src.bitsPerSample && !dspActive_.load();
        const bool changed = bp != infoBitPerfect_ || infoOutput_.sampleRate != outConfig_.sampleRate ||
                             infoOutput_.format != outConfig_.format || infoOutput_.osResamples != outConfig_.osResamples;
        infoBitPerfect_ = bp;
        infoOutput_ = outConfig_;
        if (!changed) return;
    } catch (...) {
        return;
    }
    OutputConfig cfg;
    bool bp;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        cfg = infoOutput_;
        bp = infoBitPerfect_;
    }
    notify([&](EngineListener& l) { l.onOutputChanged(cfg, bp); });
}

// ============================================================================ decoder thread

void AudioEngine::decoderLoop() noexcept {
    for (;;) {
        try {
            {
                std::unique_lock<std::mutex> lk(decMutex_);
                decBusy_ = false;
                decIdleCv_.notify_all();
                decCv_.wait(lk, [&] { return decQuit_ || (decRun_ && decStatus_ == DecodeStatus::Running && current_ && ring_); });
                if (decQuit_) return;
                decBusy_ = true;
            }
            decodeStep();
        } catch (...) {
            uint64_t id = 0;
            try {
                std::lock_guard<std::mutex> lk(decMutex_);
                decStatus_ = DecodeStatus::Failed;
                id = current_.source.id;
            } catch (...) {
            }
            post(CmdDecodeFailed{id, Error{ErrorCode::Internal, "decoder thread exception"}});
        }
    }
}

bool AudioEngine::pushMarker(const Marker& m) noexcept { return markers_->write(&m, 1) == 1; }

void AudioEngine::decodeStep() {
    const uint16_t ch = outFormat_.channels;
    const size_t need = static_cast<size_t>(cfg_.decodeChunkFrames) * ch;
    if (ring_->writable() < need) {
        // Ring full (normal while paused or ahead of the device): wait a little, but wake for park/quit.
        std::unique_lock<std::mutex> lk(decMutex_);
        decCv_.wait_for(lk, std::chrono::milliseconds(3), [&] { return !decRun_ || decQuit_; });
        return;
    }
    auto r = current_.decoder->read(decodeBuf_.data(), cfg_.decodeChunkFrames);
    if (!r) {
        uint64_t id;
        {
            std::lock_guard<std::mutex> lk(decMutex_);
            decStatus_ = DecodeStatus::Failed;
            id = current_.source.id;
        }
        post(CmdDecodeFailed{id, r.error()});
        return;
    }
    const uint32_t frames = r.value();
    if (frames > 0) {
        ring_->write(decodeBuf_.data(), static_cast<size_t>(frames) * ch);  // space was checked above
        return;
    }

    // ---- end of stream: gapless pointer swap to the preloaded next track ----
    Slot nxt;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        nxt = std::move(next_);
    }
    if (!nxt) {
        // Don't declare the end of the queue while plenty of audio is still buffered: the app usually calls
        // setNext() right after load(), and a short track can be fully decoded before that command arrives.
        // Waiting until ~150 ms remain keeps the transition gapless in that case.
        const size_t guard = static_cast<size_t>(outFormat_.sampleRate * 0.15) * ch;
        if (ring_->readable() > guard) {
            std::unique_lock<std::mutex> lk(decMutex_);
            decAtEnd_ = true;  // lets startOutputAfterPrefill() stop waiting for more data
            decCv_.wait_for(lk, std::chrono::milliseconds(3), [&] { return !decRun_ || decQuit_ || static_cast<bool>(next_); });
            return;
        }
        Marker end;
        end.sampleIndex = ring_->totalWritten();
        end.kind = MarkerKind::End;
        if (pushMarker(end)) {
            std::lock_guard<std::mutex> lk(decMutex_);
            decStatus_ = DecodeStatus::Ended;
        }
        return;  // if the marker queue was full we simply try again on the next step
    }
    const StreamFormat nf = nxt.decoder->format();
    const bool reopen = !nf.gaplessCompatible(outFormat_) || nf.bitsPerSample > outHoldsBits_;
    Marker m;
    m.sampleIndex = ring_->totalWritten();
    m.trackId = nxt.source.id;
    m.totalFrames = nxt.decoder->totalFrames();
    m.kind = reopen ? MarkerKind::FormatBoundary : MarkerKind::TrackStart;
    if (!pushMarker(m)) {
        std::lock_guard<std::mutex> lk(decMutex_);
        if (!next_) next_ = std::move(nxt);  // retry next step
        return;
    }
    std::lock_guard<std::mutex> lk(decMutex_);
    decAtEnd_ = false;
    finished_ = std::move(current_);
    current_ = std::move(nxt);
    if (reopen) decStatus_ = DecodeStatus::WaitingReconfigure;  // the control thread reopens the output
}

// ============================================================================ render thread

void AudioEngine::onDeviceError(ErrorCode) noexcept { flagDeviceError_.store(true); }

void AudioEngine::onRender(void* dst, uint32_t frames) noexcept {
    auto* out = static_cast<uint8_t*>(dst);
    const uint16_t ch = outConfig_.channels;
    const uint32_t bpf = bytesPerSample(outConfig_.format) * ch;
    if (!ring_ || ch == 0) {
        std::memset(out, 0, static_cast<size_t>(frames) * bpf);
        return;
    }
    eqDesigns_.fetch(rtDesign_);
    uint32_t done = 0;
    while (done < frames) {
        if (rtStopped_.load(std::memory_order_relaxed)) break;
        if (!rtHasMarker_) rtHasMarker_ = markers_->read(&rtMarker_, 1) == 1;
        uint64_t limit = frames - done;
        if (rtHasMarker_) {
            const uint64_t readPos = ring_->totalRead();
            if (rtMarker_.sampleIndex <= readPos) {
                rtHasMarker_ = false;
                switch (rtMarker_.kind) {
                    case MarkerKind::TrackStart:
                        heardTrack_.store(rtMarker_.trackId);
                        heardTotalFrames_.store(rtMarker_.totalFrames);
                        trackFrames_.store(rtMarker_.startFrame);
                        flagTrackChanged_.store(true);
                        break;
                    case MarkerKind::FormatBoundary:
                        heardTrack_.store(rtMarker_.trackId);
                        heardTotalFrames_.store(rtMarker_.totalFrames);
                        trackFrames_.store(0);
                        rtStopped_.store(true);
                        flagTrackChanged_.store(true);
                        flagReconfigure_.store(true);
                        break;
                    case MarkerKind::End:
                        rtStopped_.store(true);
                        flagEnded_.store(true);
                        break;
                }
                continue;
            }
            limit = std::min<uint64_t>(limit, (rtMarker_.sampleIndex - readPos) / ch);
        }
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(limit, cfg_.maxRenderFrames));
        const size_t got = ring_->read(rtScratch_.data(), static_cast<size_t>(want) * ch) / ch;
        if (got == 0) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        renderChunk(out + static_cast<size_t>(done) * bpf, rtScratch_.data(), static_cast<uint32_t>(got));
        done += static_cast<uint32_t>(got);
        trackFrames_.fetch_add(got, std::memory_order_relaxed);
    }
    if (done < frames) std::memset(out + static_cast<size_t>(done) * bpf, 0, static_cast<size_t>(frames - done) * bpf);
}

void AudioEngine::renderChunk(uint8_t* dst, const int32_t* src, uint32_t frames) noexcept {
    const size_t n = static_cast<size_t>(frames) * outConfig_.channels;
    const SampleFormat fmt = outConfig_.format;
    if (tapOn_.load(std::memory_order_relaxed) && tap_ && n <= rtTapBuf_.size()) {
        for (size_t i = 0; i < n; ++i) rtTapBuf_[i] = static_cast<float>(src[i]) * (1.0f / 2147483648.0f);
        tap_->write(rtTapBuf_.data(), n);  // if the reader is behind, newest audio is dropped (never blocks)
    }

    if (!dspActive_.load(std::memory_order_relaxed)) {
        // Bit-perfect path: exact integer shifts / exact float scaling only.
        switch (fmt) {
            case SampleFormat::Int16: {
                auto* o = reinterpret_cast<int16_t*>(dst);
                for (size_t i = 0; i < n; ++i) o[i] = static_cast<int16_t>(src[i] >> 16);
                return;
            }
            case SampleFormat::Int24Packed:
                for (size_t i = 0; i < n; ++i) {
                    const int32_t v = src[i] >> 8;
                    dst[i * 3] = static_cast<uint8_t>(v);
                    dst[i * 3 + 1] = static_cast<uint8_t>(v >> 8);
                    dst[i * 3 + 2] = static_cast<uint8_t>(v >> 16);
                }
                return;
            case SampleFormat::Int32:
                std::memcpy(dst, src, n * sizeof(int32_t));
                return;
            case SampleFormat::Float32: {
                auto* o = reinterpret_cast<float*>(dst);
                for (size_t i = 0; i < n; ++i) o[i] = static_cast<float>(src[i]) * (1.0f / 2147483648.0f);
                return;
            }
        }
        return;
    }

    // DSP path: double precision EQ + volume, then clamp (counted) and dither to integer outputs.
    double* x = rtDsp_.data();
    for (size_t i = 0; i < n; ++i) x[i] = static_cast<double>(src[i]) * (1.0 / 2147483648.0);
    if (!rtDesign_.transparent) rtEq_.process(rtDesign_, x, frames);
    const double vol = volume_.load(std::memory_order_relaxed);
    if (vol != 1.0)
        for (size_t i = 0; i < n; ++i) x[i] *= vol;
    uint64_t clipped = 0;
    auto clampCount = [&](double v) {
        if (v > 1.0) { ++clipped; return 1.0; }
        if (v < -1.0) { ++clipped; return -1.0; }
        return v;
    };
    auto tpdf = [&]() {  // triangular dither in [-1, 1) LSB, xorshift32
        auto next = [&]() {
            rtDither_ ^= rtDither_ << 13;
            rtDither_ ^= rtDither_ >> 17;
            rtDither_ ^= rtDither_ << 5;
            return static_cast<double>(rtDither_ >> 8) * (1.0 / 16777216.0);
        };
        return next() - next();
    };
    switch (fmt) {
        case SampleFormat::Float32: {
            auto* o = reinterpret_cast<float*>(dst);
            for (size_t i = 0; i < n; ++i) o[i] = static_cast<float>(clampCount(x[i]));
            break;
        }
        case SampleFormat::Int16: {
            auto* o = reinterpret_cast<int16_t*>(dst);
            for (size_t i = 0; i < n; ++i) {
                const double v = std::nearbyint(clampCount(x[i]) * 32767.0 + tpdf());
                o[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
            }
            break;
        }
        case SampleFormat::Int24Packed:
            for (size_t i = 0; i < n; ++i) {
                const double v = std::nearbyint(clampCount(x[i]) * 8388607.0 + tpdf());
                const int32_t s = static_cast<int32_t>(std::clamp(v, -8388608.0, 8388607.0));
                dst[i * 3] = static_cast<uint8_t>(s);
                dst[i * 3 + 1] = static_cast<uint8_t>(s >> 8);
                dst[i * 3 + 2] = static_cast<uint8_t>(s >> 16);
            }
            break;
        case SampleFormat::Int32: {
            auto* o = reinterpret_cast<int32_t*>(dst);
            for (size_t i = 0; i < n; ++i)
                o[i] = static_cast<int32_t>(std::clamp(std::nearbyint(clampCount(x[i]) * 2147483647.0), -2147483648.0, 2147483647.0));
            break;
        }
    }
    if (clipped) clipped_.fetch_add(clipped, std::memory_order_relaxed);
}

}  // namespace groove
