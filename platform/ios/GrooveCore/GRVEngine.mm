// Objective-C++ bridge. Memory rules:
//  * The C++ engine is owned by std::unique_ptr inside the Obj-C object (RAII; destroyed in -shutdown/dealloc).
//  * The C++ listener holds the Obj-C object through a __weak pointer, and the engine holds the listener
//    through a std::weak_ptr — no retain cycle in either direction.
//  * Listener callbacks hop to the main queue with dispatch_async capturing only the weak reference.
//  * PCM never becomes an NSData: the tap copies into memory the caller owns.
#import "GRVEngine.h"

#import <AVFoundation/AVFoundation.h>

#include <memory>

#include "CoreAudioOutput.h"
#include "groove/AudioEngine.h"

@interface GRVSnapshot ()
- (instancetype)initWithSnapshot:(const groove::EngineSnapshot&)s;
@end

@implementation GRVSnapshot
- (instancetype)initWithSnapshot:(const groove::EngineSnapshot&)s {
    if ((self = [super init])) {
        _state = static_cast<GRVPlaybackState>(s.state);
        _trackId = s.trackId;
        _position = s.positionSeconds;
        _duration = s.durationSeconds;
        _sourceSampleRate = s.source.sampleRate;
        _sourceBits = s.source.bitsPerSample;
        _outputSampleRate = s.output.sampleRate;
        _bitPerfect = s.bitPerfect;
        _dspActive = s.dspActive;
        _osResamples = s.output.osResamples;
        _fromDSD = s.source.fromDsd;
        _underruns = s.underruns;
        _clippedSamples = s.clippedSamples;
        _eqHeadroomDb = s.eqHeadroomDb;
    }
    return self;
}
@end

namespace {

class ObjcListener final : public groove::EngineListener {
public:
    explicit ObjcListener(GRVEngine* owner) : owner_(owner) {}
    void onStateChanged(groove::PlaybackState s, uint64_t trackId) noexcept override {
        post(^(GRVEngine*, id<GRVEngineDelegate> d) {
            if ([d respondsToSelector:@selector(engineStateDidChange:trackId:)]) [d engineStateDidChange:static_cast<GRVPlaybackState>(s) trackId:trackId];
        });
    }
    void onTrackStarted(uint64_t trackId) noexcept override {
        post(^(GRVEngine*, id<GRVEngineDelegate> d) {
            if ([d respondsToSelector:@selector(engineDidStartTrack:)]) [d engineDidStartTrack:trackId];
        });
    }
    void onTrackFailed(uint64_t trackId, groove::ErrorCode c, const std::string& msg) noexcept override {
        NSString* m = [NSString stringWithUTF8String:msg.c_str()] ?: @"";
        post(^(GRVEngine*, id<GRVEngineDelegate> d) {
            if ([d respondsToSelector:@selector(engineDidFailTrack:code:message:)]) [d engineDidFailTrack:trackId code:static_cast<NSInteger>(c) message:m];
        });
    }
    void onQueueEnded() noexcept override {
        post(^(GRVEngine*, id<GRVEngineDelegate> d) {
            if ([d respondsToSelector:@selector(engineQueueDidEnd)]) [d engineQueueDidEnd];
        });
    }
    void onOutputChanged(const groove::OutputConfig& cfg, bool bp) noexcept override {
        const uint32_t rate = cfg.sampleRate;
        const bool os = cfg.osResamples;
        post(^(GRVEngine*, id<GRVEngineDelegate> d) {
            if ([d respondsToSelector:@selector(engineOutputDidChangeSampleRate:osResamples:bitPerfect:)])
                [d engineOutputDidChangeSampleRate:rate osResamples:os bitPerfect:bp];
        });
    }

private:
    void post(void (^block)(GRVEngine*, id<GRVEngineDelegate>)) noexcept {
        __weak GRVEngine* weakOwner = owner_;
        dispatch_async(dispatch_get_main_queue(), ^{
            GRVEngine* strong = weakOwner;  // nil if the engine was released meanwhile
            id<GRVEngineDelegate> d = strong.delegate;
            if (strong && d) block(strong, d);
        });
    }
    __weak GRVEngine* owner_;
};

}  // namespace

@implementation GRVEngine {
    std::unique_ptr<groove::AudioEngine> _engine;
    std::shared_ptr<ObjcListener> _listener;
    groove::ios::CoreAudioOutput* _output;  // owned by _engine
    id _routeObserver;
    id _resetObserver;
}

- (nullable instancetype)initWithRingSeconds:(double)ringSeconds {
    if (!(self = [super init])) return nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ groove::ios::registerAppleDecoder(); });

    auto out = std::make_unique<groove::ios::CoreAudioOutput>();
    _output = out.get();
    groove::EngineConfig cfg;
    if (ringSeconds > 0) cfg.ringSeconds = ringSeconds;
    auto r = groove::AudioEngine::create(cfg, std::move(out), std::make_shared<groove::PathOpener>());
    if (!r) {
        NSLog(@"Groove engine failed to start: %s", r.error().message.c_str());
        return nil;
    }
    _engine = std::move(r).value();
    _listener = std::make_shared<ObjcListener>(self);
    _engine->setListener(_listener);

    // Headphones unplugged / DAC removed / media services reset → the engine reopens the output on its own
    // control thread (AudioEngine::recoverDevice). The observer blocks capture self weakly.
    __weak GRVEngine* weakSelf = self;
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    _routeObserver = [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* n) {
        const auto reason = static_cast<AVAudioSessionRouteChangeReason>([n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
        GRVEngine* s = weakSelf;
        if (!s || !s->_output) return;  // main queue, like -shutdown: no race on _output
        if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) [s pause];  // don't blast the speaker
        if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable || reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable)
            s->_output->notifyRouteLost();  // reopen at the new device's rate
    }];
    _resetObserver = [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
        GRVEngine* s = weakSelf;
        if (s && s->_output) s->_output->notifyRouteLost();
    }];
    return self;
}

- (void)dealloc { [self shutdown]; }

/// Call on the main thread (dealloc of a main-thread-owned object is).
- (void)shutdown {
    if (_routeObserver) [NSNotificationCenter.defaultCenter removeObserver:_routeObserver];
    if (_resetObserver) [NSNotificationCenter.defaultCenter removeObserver:_resetObserver];
    _routeObserver = nil;
    _resetObserver = nil;
    _engine.reset();  // stops audio, joins threads
    _listener.reset();
    _output = nullptr;
}

- (void)loadTrack:(uint64_t)trackId path:(NSString*)path autoplay:(BOOL)autoplay startAt:(double)seconds {
    if (!_engine) return;
    groove::LoadOptions o;
    o.autoplay = autoplay;
    o.startSeconds = seconds;
    _engine->load(groove::TrackSource{trackId, path.fileSystemRepresentation}, o);
}

- (void)setNextTrack:(uint64_t)trackId path:(nullable NSString*)path {
    if (!_engine) return;
    if (path) _engine->setNext(groove::TrackSource{trackId, path.fileSystemRepresentation});
    else _engine->setNext(std::nullopt);
}

- (void)play { if (_engine) _engine->play(); }
- (void)pause { if (_engine) _engine->pause(); }
- (void)stop { if (_engine) _engine->stop(); }
- (void)seekTo:(double)seconds { if (_engine) _engine->seek(seconds); }
- (void)setVolume:(double)linear { if (_engine) _engine->setVolume(linear); }

- (void)setEqEnabled:(BOOL)enabled gains:(NSArray<NSNumber*>*)gains preamp:(double)preampDb guaranteedHeadroom:(BOOL)guaranteed {
    if (!_engine || gains.count != groove::kEqBands) return;
    groove::EqSettings s;
    s.enabled = enabled;
    for (size_t i = 0; i < groove::kEqBands; ++i) s.bands[i].gainDb = gains[i].doubleValue;
    s.preampDb = preampDb;
    s.headroom = guaranteed ? groove::HeadroomMode::Guaranteed : groove::HeadroomMode::PeakResponse;
    _engine->setEq(s);
}

- (GRVSnapshot*)snapshot {
    return [[GRVSnapshot alloc] initWithSnapshot:_engine ? _engine->snapshot() : groove::EngineSnapshot{}];
}

- (void)setTapEnabled:(BOOL)enabled { if (_engine) _engine->enableTap(enabled); }

- (NSInteger)readTapInto:(float*)buffer capacity:(NSInteger)maxSamples {
    if (!_engine || !buffer || maxSamples <= 0) return 0;
    return static_cast<NSInteger>(_engine->readTap(buffer, static_cast<size_t>(maxSamples)));
}

@end
