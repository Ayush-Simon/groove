// Groove AudioEngine — thread-safe playback core shared by Android and iOS.
//
// ┌──────────── THREADS ─────────────────────────────────────────────────────────────────────────┐
// │ Caller threads (UI, JNI, Obj-C++)  → post commands, never block (except waitIdle()).          │
// │ Control thread  (owned)   → the ONLY thread that changes PlaybackState. Serially executes      │
// │                             commands from a mutex-protected queue; opens files and outputs.   │
// │ Decoder thread  (owned)   → decodes ahead into a lock-free SPSC ring buffer (≈2 s). Swaps to   │
// │                             the preloaded next track at end of stream (gapless).              │
// │ Render thread   (OS)      → AudioOutput callback. Reads the ring, optional EQ/volume, converts │
// │                             to the device format. No locks, no allocation, no I/O, noexcept.  │
// └──────────────────────────────────────────────────────────────────────────────────────────────┘
//
// ┌──────────── STATE MACHINE (validated in transitionTo) ──────────────────────────────────────┐
// │ Stopped ──load──▶ Loading ──ok,autoplay──▶ Playing ◀──play/pause──▶ Paused                  │
// │                      │ ──ok,!autoplay──▶ Paused                                             │
// │                      └─fail──▶ Error ──load──▶ Loading       Error ──stop──▶ Stopped         │
// │ Playing/Paused ──stop──▶ Stopped     Playing ──queue ended──▶ Stopped                        │
// │ Playing/Paused ──load──▶ Loading   (seek stays in Playing/Paused; the pipeline is refilled)  │
// │ Playing ──device lost, 3 reopen attempts failed──▶ Error                                     │
// │ Any ──decode/storage failure──▶ skip to the preloaded next track if there is one, else Error │
// └──────────────────────────────────────────────────────────────────────────────────────────────┘
//
// GAPLESS: the app calls setNext() with the following track as soon as a track starts. The engine
// opens and primes its decoder ahead of time ("double buffering"); at end of stream the decoder thread
// swaps its current/next pointers and keeps writing into the same ring, so the device never sees a gap.
// setNext() must arrive before the last ~150 ms of the current track are buffered; later than that the
// next track still plays, but after a short restart instead of gaplessly.
// If the next track has a different sample rate or channel count, the output must be reopened at the
// new rate to stay bit-perfect; that boundary has a short, clean pause instead of a resample.
//
// BIT-PERFECT: with the EQ transparent and volume at 1.0, samples go from the decoder to the device
// buffer with no arithmetic (exact shifts only). Whether the OS then leaves them alone is up to the
// platform — EngineSnapshot::bitPerfect is only true when the output reports no OS resampling, the
// device rate equals the source rate and the device format can hold every source bit.
//
// ERRORS: no exception escapes this class. Public methods are noexcept; internal thread bodies catch
// everything and convert it to ErrorCode::Internal + the Error state.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "groove/AudioOutput.h"
#include "groove/ByteSource.h"
#include "groove/Decoder.h"
#include "groove/Equalizer.h"
#include "groove/Result.h"
#include "groove/SpscRing.h"
#include "groove/Types.h"

namespace groove {

struct EngineConfig {
    double ringSeconds = 2.0;       // decoded audio buffered ahead of the device
    double prefillSeconds = 0.4;    // buffered before the output starts (avoids a cold-start underrun)
    uint32_t decodeChunkFrames = 4096;
    uint32_t maxRenderFrames = 8192;  // largest callback the scratch buffers are sized for (bigger ones are chunked)
    int deviceReopenAttempts = 3;
};

struct LoadOptions {
    bool autoplay = true;
    double startSeconds = 0.0;
};

/// Immutable copy of the engine's public state. Safe to call from any thread at any rate.
struct EngineSnapshot {
    PlaybackState state = PlaybackState::Stopped;
    uint64_t trackId = 0;          // the track you are HEARING (not the one being decoded)
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    StreamFormat source{};
    OutputConfig output{};
    bool bitPerfect = false;
    bool dspActive = false;
    double eqHeadroomDb = 0.0;
    uint64_t underruns = 0;
    uint64_t clippedSamples = 0;
    ErrorCode lastError = ErrorCode::None;
    std::string lastErrorMessage;
};

/// Callbacks run on the engine's control thread. The engine holds the listener weakly, so a UI object
/// that owns the engine and listens to it never forms a retain cycle. Implementations should hop to their
/// UI thread and return quickly.
class EngineListener {
public:
    virtual ~EngineListener() = default;
    virtual void onStateChanged(PlaybackState /*state*/, uint64_t /*trackId*/) noexcept {}
    virtual void onTrackStarted(uint64_t /*trackId*/) noexcept {}   // also fires on gapless transitions
    virtual void onTrackFailed(uint64_t /*trackId*/, ErrorCode /*code*/, const std::string& /*message*/) noexcept {}
    virtual void onQueueEnded() noexcept {}
    virtual void onOutputChanged(const OutputConfig& /*cfg*/, bool /*bitPerfect*/) noexcept {}
};

class AudioEngine final : private AudioOutput::Callback {
    struct PrivateTag { explicit PrivateTag() = default; };  // only AudioEngine can name this type

public:
    static Result<std::unique_ptr<AudioEngine>> create(EngineConfig config, std::unique_ptr<AudioOutput> output,
                                                       std::shared_ptr<SourceOpener> opener) noexcept;
    AudioEngine(PrivateTag, EngineConfig config, std::unique_ptr<AudioOutput> output, std::shared_ptr<SourceOpener> opener);
    ~AudioEngine() override;  // stops playback and joins both threads
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ---- Commands: thread-safe, non-blocking, executed in order on the control thread ----
    void load(TrackSource track, LoadOptions opts = {}) noexcept;
    /// Preload the track that should follow the current one without a gap. nullopt clears it.
    void setNext(std::optional<TrackSource> track) noexcept;
    void play() noexcept;
    void pause() noexcept;
    void stop() noexcept;
    void seek(double seconds) noexcept;
    void setEq(const EqSettings& eq) noexcept;
    /// 1.0 = unity (bit-perfect). Prefer the device's hardware volume for bit-perfect listening.
    void setVolume(double linear) noexcept;
    void setListener(std::weak_ptr<EngineListener> listener) noexcept;

    /// PCM tap for meters / visualisers: copies the most recent audio sent to the device (interleaved float,
    /// output channel count) into caller-owned memory. Lock-free on the audio side; call from one thread
    /// at a time. Bridges hand it a JNI DirectByteBuffer / Swift-owned buffer, so no GC or ARC is involved.
    void enableTap(bool on) noexcept { tapOn_.store(on); }
    size_t readTap(float* dst, size_t maxSamples) noexcept;

    /// Blocks until every command posted before this call has been executed (bridges / tests).
    bool waitIdle(std::chrono::milliseconds timeout) noexcept;
    EngineSnapshot snapshot() const noexcept;

    /// Exposed for tests: is `to` reachable from `from` in the state machine?
    static bool isTransitionAllowed(PlaybackState from, PlaybackState to) noexcept;

private:
    // ----- commands -----
    struct CmdLoad { TrackSource track; LoadOptions opts; };
    struct CmdSetNext { std::optional<TrackSource> track; };
    struct CmdPlay {};
    struct CmdPause {};
    struct CmdStop {};
    struct CmdSeek { double seconds; };
    struct CmdEq { EqSettings eq; };
    struct CmdVolume { double v; };
    struct CmdDecodeFailed { uint64_t trackId; Error error; };
    struct CmdShutdown {};
    using Command = std::variant<CmdLoad, CmdSetNext, CmdPlay, CmdPause, CmdStop, CmdSeek, CmdEq, CmdVolume,
                                 CmdDecodeFailed, CmdShutdown>;

    // ----- ring-buffer markers (decoder → render thread) -----
    enum class MarkerKind : uint8_t { TrackStart, FormatBoundary, End };
    struct Marker {
        uint64_t sampleIndex = 0;   // position in the ring's total-written count
        uint64_t trackId = 0;
        uint64_t startFrame = 0;    // first frame of the track at this marker (non-zero after a seek)
        uint64_t totalFrames = 0;
        MarkerKind kind = MarkerKind::TrackStart;
    };

    struct Slot {
        std::unique_ptr<Decoder> decoder;
        TrackSource source;
        explicit operator bool() const noexcept { return static_cast<bool>(decoder); }
    };

    enum class DecodeStatus : uint8_t { Idle, Running, WaitingReconfigure, Ended, Failed };

    void post(Command c) noexcept;
    void controlLoop() noexcept;
    void handle(Command& c);
    void onLoad(CmdLoad& c);
    void onSetNext(CmdSetNext& c);
    void onPlay();
    void onPause();
    void onStop();
    void onSeek(double seconds);
    void onEq(const EqSettings& eq);
    void onVolume(double v);
    void onDecodeFailed(const CmdDecodeFailed& c);
    void pollRealtimeFlags();

    // world control (control thread only)
    void stopTheWorld() noexcept;                // output stopped + decoder parked
    void resetPipeline(const Slot& heard, uint64_t startFrame) noexcept;  // ring + markers + RT counters
    Result<void> configureOutput(const StreamFormat& fmt) noexcept;
    Result<void> startOutputAfterPrefill() noexcept;
    bool recoverDevice() noexcept;
    void updateBitPerfect() noexcept;
    Result<Slot> openSlot(const TrackSource& t) noexcept;
    void startSlot(Slot slot, uint64_t startFrame, bool autoplay);
    bool needsReopen(const StreamFormat& f) const noexcept;
    void rememberSource(const TrackSource& t);
    void failAndMaybeSkip(uint64_t trackId, const Error& err);
    bool transitionTo(PlaybackState s) noexcept;
    void setError(const Error& err) noexcept;
    template <typename F> void notify(F&& f) noexcept;

    // decoder thread
    void decoderLoop() noexcept;
    void decodeStep();
    void parkDecoder() noexcept;
    void runDecoder() noexcept;
    bool pushMarker(const Marker& m) noexcept;

    // render thread (AudioOutput::Callback)
    void onRender(void* dst, uint32_t frames) noexcept override;
    void onDeviceError(ErrorCode code) noexcept override;
    void renderChunk(uint8_t* dst, const int32_t* src, uint32_t frames) noexcept;

    const EngineConfig cfg_;
    std::unique_ptr<AudioOutput> output_;
    std::shared_ptr<SourceOpener> opener_;

    // command queue
    mutable std::mutex qMutex_;
    std::condition_variable qCv_;
    std::condition_variable idleCv_;
    std::deque<Command> queue_;
    uint64_t posted_ = 0, processed_ = 0;

    std::thread controlThread_;
    std::thread decoderThread_;

    // decoder-thread shared state (guarded by decMutex_)
    std::mutex decMutex_;
    std::condition_variable decCv_;
    std::condition_variable decIdleCv_;
    bool decRun_ = false;
    bool decBusy_ = false;
    bool decQuit_ = false;
    DecodeStatus decStatus_ = DecodeStatus::Idle;
    bool decAtEnd_ = false;  // current track fully decoded; holding before declaring the end of the queue
    Slot current_;    // being decoded (decoder thread while running; control thread while parked)
    Slot next_;       // preloaded (guarded by decMutex_)
    Slot finished_;   // decoded but maybe still audible; kept so a seek in the old track still works
    std::vector<int32_t> decodeBuf_;

    // pipeline (replaced only while the world is stopped)
    std::unique_ptr<SpscRing<int32_t>> ring_;
    std::unique_ptr<SpscRing<Marker>> markers_;
    StreamFormat outFormat_{};      // format the output is currently configured for
    OutputConfig outConfig_{};
    bool outputOpen_ = false;
    bool outputRunning_ = false;
    uint16_t outHoldsBits_ = 0;     // how many source bits the device format carries exactly

    // PCM tap (render thread writes lock-free; readTap() consumes under tapMutex_)
    std::unique_ptr<SpscRing<float>> tap_;
    std::mutex tapMutex_;
    std::atomic<bool> tapOn_{false};
    std::vector<float> rtTapBuf_;

    // render-thread-only state
    std::vector<int32_t> rtScratch_;
    std::vector<double> rtDsp_;
    EqProcessor rtEq_;
    EqDesign rtDesign_;
    Marker rtMarker_{};
    bool rtHasMarker_ = false;
    uint32_t rtDither_ = 0x9E3779B9u;

    // cross-thread atomics (render ↔ control)
    std::atomic<uint64_t> heardTrack_{0};
    std::atomic<uint64_t> trackFrames_{0};        // frames of the heard track rendered (incl. start offset)
    std::atomic<uint64_t> heardTotalFrames_{0};
    std::atomic<bool> flagTrackChanged_{false};
    std::atomic<bool> flagReconfigure_{false};
    std::atomic<bool> flagEnded_{false};
    std::atomic<bool> flagDeviceError_{false};
    std::atomic<bool> rtStopped_{false};           // render thread hit a boundary/end and is outputting silence
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<bool> dspActive_{false};
    std::atomic<double> volume_{1.0};
    TripleBuffer<EqDesign> eqDesigns_;

    // control-thread state
    std::atomic<PlaybackState> state_{PlaybackState::Stopped};
    EqSettings eqSettings_{};
    double volumeValue_ = 1.0;
    std::deque<TrackSource> recent_;          // last few sources, to reopen a track for seeking
    std::weak_ptr<EngineListener> listener_;  // control thread reads; guarded by infoMutex_

    // snapshot info (guarded by infoMutex_; never touched by the render thread)
    mutable std::mutex infoMutex_;
    StreamFormat infoSource_{};
    OutputConfig infoOutput_{};
    bool infoBitPerfect_ = false;
    double infoHeadroomDb_ = 0.0;
    Error lastError_{ErrorCode::None, {}};
};

}  // namespace groove
