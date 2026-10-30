#include "AAudioOutput.h"

#include <android/log.h>

#include <string>

namespace groove::android {

namespace {
constexpr const char* kTag = "GrooveAAudio";

SampleFormat toSampleFormat(aaudio_format_t f) {
    switch (f) {
        case AAUDIO_FORMAT_PCM_I16: return SampleFormat::Int16;
        case AAUDIO_FORMAT_PCM_I24_PACKED: return SampleFormat::Int24Packed;  // devices on API 31+
        case AAUDIO_FORMAT_PCM_I32: return SampleFormat::Int32;
        default: return SampleFormat::Float32;
    }
}
}  // namespace

aaudio_data_callback_result_t AAudioOutput::dataCallback(AAudioStream*, void* user, void* audio, int32_t frames) {
    // Real-time thread: straight into the engine (noexcept, lock-free, allocation-free).
    auto* self = static_cast<AAudioOutput*>(user);
    if (frames > 0) self->cb_->onRender(audio, static_cast<uint32_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioOutput::errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    // AAudio forbids closing/reopening the stream from this callback. Raise a flag; the engine's control
    // thread reopens the output (AudioEngine::recoverDevice).
    __android_log_print(ANDROID_LOG_WARN, kTag, "stream error %s", AAudio_convertResultToText(error));
    auto* self = static_cast<AAudioOutput*>(user);
    if (self->cb_) self->cb_->onDeviceError(ErrorCode::DeviceLost);
}

Result<OutputConfig> AAudioOutput::tryOpen(const OutputRequest& req, aaudio_format_t fmt, bool exclusive) noexcept {
    AAudioStreamBuilder* raw = nullptr;
    if (AAudio_createStreamBuilder(&raw) != AAUDIO_OK) return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "createStreamBuilder");
    std::unique_ptr<AAudioStreamBuilder, BuilderDeleter> b(raw);
    AAudioStreamBuilder_setDirection(raw, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(raw, static_cast<int32_t>(req.sampleRate));   // never resampled by Groove
    AAudioStreamBuilder_setChannelCount(raw, req.channels);
    AAudioStreamBuilder_setFormat(raw, fmt);
    // EXCLUSIVE (MMAP) can bypass the system mixer but needs LOW_LATENCY; SHARED uses POWER_SAVING buffers,
    // which is kinder to the battery for plain music playback.
    AAudioStreamBuilder_setSharingMode(raw, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(raw, exclusive ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
    // API 28+ calls: weakly linked (__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__) and guarded at run time,
    // so the same library still loads on Android 8.x.
    if (__builtin_available(android 28, *)) {
        AAudioStreamBuilder_setUsage(raw, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(raw, AAUDIO_CONTENT_TYPE_MUSIC);
    }
    AAudioStreamBuilder_setDataCallback(raw, &AAudioOutput::dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(raw, &AAudioOutput::errorCallback, this);

    AAudioStream* s = nullptr;
    const aaudio_result_t r = AAudioStreamBuilder_openStream(raw, &s);
    if (r != AAUDIO_OK || !s) return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, AAudio_convertResultToText(r));
    stream_.reset(s);

    // Music, not a game: trade latency for safety against underruns.
    const int32_t cap = AAudioStream_getBufferCapacityInFrames(s);
    if (cap > 0) AAudioStream_setBufferSizeInFrames(s, cap);

    OutputConfig c;
    c.sampleRate = static_cast<uint32_t>(AAudioStream_getSampleRate(s));
    c.channels = static_cast<uint16_t>(AAudioStream_getChannelCount(s));
    c.format = toSampleFormat(AAudioStream_getFormat(s));
    c.framesPerBurst = static_cast<uint32_t>(AAudioStream_getFramesPerBurst(s));
    c.exclusive = AAudioStream_getSharingMode(s) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    // Honest reporting: in SHARED mode the Android mixer touches every sample (and converts to its own rate),
    // unless Android 14+ has put a USB DAC into bit-perfect mixer mode for this format.
    const bool bitPerfectMixer = bitPerfectMixer_.load();
    const bool rateMatches = c.sampleRate == mixerRate_.load() || bitPerfectMixer;
    c.osResamples = !((c.exclusive || bitPerfectMixer) && rateMatches);
    c.deviceName = "AAudio device " + std::to_string(AAudioStream_getDeviceId(s));
    return c;
}

Result<OutputConfig> AAudioOutput::open(const OutputRequest& req, Callback* cb) noexcept {
    close();
    if (!cb) return Result<OutputConfig>::fail(ErrorCode::InvalidArgument, "no callback");
    cb_ = cb;
    // Pick the narrowest device format that carries every source bit.
    aaudio_format_t fmt = AAUDIO_FORMAT_PCM_FLOAT;  // exact for ≤ 24-bit sources
    if (req.sourceBits <= 16) fmt = AAUDIO_FORMAT_PCM_I16;
    else if (apiLevel_ >= 31) fmt = req.sourceBits <= 24 ? AAUDIO_FORMAT_PCM_I24_PACKED : AAUDIO_FORMAT_PCM_I32;
    const aaudio_format_t fallbacks[] = {fmt, AAUDIO_FORMAT_PCM_FLOAT};
    for (aaudio_format_t f : fallbacks) {
        for (bool exclusive : {req.preferExclusive, false}) {
            auto r = tryOpen(req, f, exclusive);
            if (r && r.value().sampleRate == req.sampleRate && r.value().channels == req.channels) {
                cfg_ = r.value();
                __android_log_print(ANDROID_LOG_INFO, kTag, "opened %u Hz, %u ch, fmt %d, %s, osResamples=%d", cfg_.sampleRate, cfg_.channels,
                                    static_cast<int>(cfg_.format), cfg_.exclusive ? "EXCLUSIVE" : "SHARED", cfg_.osResamples ? 1 : 0);
                return cfg_;
            }
            stream_.reset();
            if (!req.preferExclusive) break;
        }
    }
    return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "AAudio refused every configuration");
}

Result<void> AAudioOutput::start() noexcept {
    if (!stream_) return Result<void>::fail(ErrorCode::InvalidState, "not open");
    const aaudio_result_t r = AAudioStream_requestStart(stream_.get());
    if (r != AAUDIO_OK) return Result<void>::fail(ErrorCode::DeviceLost, AAudio_convertResultToText(r));
    return Result<void>::success();
}

Result<void> AAudioOutput::stop() noexcept {
    if (!stream_) return Result<void>::success();
    AAudioStream* s = stream_.get();
    if (AAudioStream_requestStop(s) != AAUDIO_OK) return Result<void>::success();  // already stopped / disconnected
    // Synchronous contract: wait until the callback can no longer run.
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING, next = AAUDIO_STREAM_STATE_UNINITIALIZED;
    for (int i = 0; i < 20 && state != AAUDIO_STREAM_STATE_STOPPED && state != AAUDIO_STREAM_STATE_DISCONNECTED; ++i) {
        if (AAudioStream_waitForStateChange(s, state, &next, 100'000'000LL) != AAUDIO_OK) break;
        state = next;
    }
    return Result<void>::success();
}

void AAudioOutput::close() noexcept {
    if (stream_) {
        (void)stop();
        stream_.reset();  // AAudioStream_close
    }
    cb_ = nullptr;
}

}  // namespace groove::android
