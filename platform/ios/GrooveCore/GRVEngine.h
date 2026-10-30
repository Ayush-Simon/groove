// Objective-C face of the C++ engine. Pure Objective-C (no C++ types) so Swift imports it through the
// bridging header / module map. The implementation (GRVEngine.mm) is Objective-C++.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, GRVPlaybackState) {   // mirrors groove::PlaybackState
    GRVPlaybackStateStopped = 0,
    GRVPlaybackStateLoading,
    GRVPlaybackStatePlaying,
    GRVPlaybackStatePaused,
    GRVPlaybackStateError,
};

@interface GRVSnapshot : NSObject
@property (nonatomic, readonly) GRVPlaybackState state;
@property (nonatomic, readonly) uint64_t trackId;
@property (nonatomic, readonly) double position;
@property (nonatomic, readonly) double duration;
@property (nonatomic, readonly) uint32_t sourceSampleRate;
@property (nonatomic, readonly) uint16_t sourceBits;
@property (nonatomic, readonly) uint32_t outputSampleRate;
@property (nonatomic, readonly) BOOL bitPerfect;
@property (nonatomic, readonly) BOOL dspActive;
@property (nonatomic, readonly) BOOL osResamples;
@property (nonatomic, readonly) BOOL fromDSD;
@property (nonatomic, readonly) uint64_t underruns;
@property (nonatomic, readonly) uint64_t clippedSamples;
@property (nonatomic, readonly) double eqHeadroomDb;
@end

/// All delegate calls arrive on the main queue. The engine holds the delegate weakly.
@protocol GRVEngineDelegate <NSObject>
@optional
- (void)engineStateDidChange:(GRVPlaybackState)state trackId:(uint64_t)trackId;
- (void)engineDidStartTrack:(uint64_t)trackId;
- (void)engineDidFailTrack:(uint64_t)trackId code:(NSInteger)code message:(NSString *)message;
- (void)engineQueueDidEnd;
- (void)engineOutputDidChangeSampleRate:(uint32_t)rate osResamples:(BOOL)osResamples bitPerfect:(BOOL)bitPerfect;
@end

@interface GRVEngine : NSObject
/// nil if the engine couldn't start (logged). Configures AVAudioSession for playback.
- (nullable instancetype)initWithRingSeconds:(double)ringSeconds NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@property (nonatomic, weak, nullable) id<GRVEngineDelegate> delegate;

- (void)loadTrack:(uint64_t)trackId path:(NSString *)path autoplay:(BOOL)autoplay startAt:(double)seconds;
- (void)setNextTrack:(uint64_t)trackId path:(nullable NSString *)path;
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekTo:(double)seconds;
- (void)setVolume:(double)linear;
/// gains: exactly 10 NSNumber dB values (31 Hz … 16 kHz).
- (void)setEqEnabled:(BOOL)enabled gains:(NSArray<NSNumber *> *)gains preamp:(double)preampDb guaranteedHeadroom:(BOOL)guaranteed;
- (GRVSnapshot *)snapshot;

/// Visualiser tap: copies the newest float samples into caller-owned memory (no NSData/ARC per call).
- (void)setTapEnabled:(BOOL)enabled;
- (NSInteger)readTapInto:(float *)buffer capacity:(NSInteger)maxSamples;

/// Stops audio and joins the engine threads. Also called from dealloc.
- (void)shutdown;
@end

NS_ASSUME_NONNULL_END
