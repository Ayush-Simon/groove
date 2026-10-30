// Objective-C++: AVAudioSession is Obj-C; the render callback below is plain C — no Obj-C messaging, no ARC
// traffic, no locks and no allocation ever happen on the real-time thread.
#import <AVFoundation/AVFoundation.h>

#include "CoreAudioOutput.h"
#include "groove/Decoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace groove::ios {

OSStatus CoreAudioOutput::render(void* user, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<CoreAudioOutput*>(user);
    if (io->mNumberBuffers > 0 && io->mBuffers[0].mData) self->cb_->onRender(io->mBuffers[0].mData, frames);  // interleaved
    return noErr;
}

Result<OutputConfig> CoreAudioOutput::open(const OutputRequest& req, Callback* cb) noexcept {
    close();
    if (!cb) return Result<OutputConfig>::fail(ErrorCode::InvalidArgument, "no callback");
    cb_ = cb;
    @autoreleasepool {
        // Ask the hardware for the source rate. A USB DAC switches to it; the built-in speaker / Bluetooth
        // stay at their fixed rate and iOS converts — we read the result back and report it honestly.
        AVAudioSession* s = [AVAudioSession sharedInstance];
        NSError* err = nil;
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
        [s setPreferredSampleRate:req.sampleRate error:&err];
        [s setPreferredIOBufferDuration:0.02 error:&err];
        if (![s setActive:YES error:&err]) return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "AVAudioSession activation failed");
        const double hwRate = s.sampleRate;

        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "RemoteIO");

        // Float32 interleaved: exact for every ≤24-bit integer sample (the engine reports >24-bit as not exact).
        AudioStreamBasicDescription f{};
        f.mSampleRate = req.sampleRate;  // the unit converts if the hardware runs at another rate (reported below)
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mChannelsPerFrame = req.channels;
        f.mBitsPerChannel = 32;
        f.mBytesPerFrame = 4 * req.channels;
        f.mFramesPerPacket = 1;
        f.mBytesPerPacket = f.mBytesPerFrame;
        AURenderCallbackStruct rc{&CoreAudioOutput::render, this};
        UInt32 maxFrames = 8192;
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
            AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rc, sizeof rc) != noErr ||
            AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames) != noErr ||
            AudioUnitInitialize(unit_) != noErr) {
            close();
            return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "RemoteIO setup");
        }
        cfg_ = OutputConfig{};
        cfg_.sampleRate = req.sampleRate;
        cfg_.channels = req.channels;
        cfg_.format = SampleFormat::Float32;
        cfg_.framesPerBurst = static_cast<uint32_t>(s.IOBufferDuration * hwRate);
        cfg_.exclusive = false;
        // With the hardware at the source rate and system volume at 100 %, iOS passes samples through unchanged.
        cfg_.osResamples = std::lround(hwRate) != static_cast<long>(req.sampleRate);
        cfg_.deviceName = s.currentRoute.outputs.firstObject.portName.UTF8String ?: "iOS output";
    }
    return cfg_;
}

Result<void> CoreAudioOutput::start() noexcept {
    if (!unit_) return Result<void>::fail(ErrorCode::InvalidState, "not open");
    if (running_) return Result<void>::success();
    if (AudioOutputUnitStart(unit_) != noErr) return Result<void>::fail(ErrorCode::DeviceLost, "AudioOutputUnitStart");
    running_ = true;
    return Result<void>::success();
}

Result<void> CoreAudioOutput::stop() noexcept {
    // AudioOutputUnitStop is synchronous: when it returns, the render callback has finished.
    if (unit_ && running_) AudioOutputUnitStop(unit_);
    running_ = false;
    return Result<void>::success();
}

void CoreAudioOutput::close() noexcept {
    (void)stop();
    if (unit_) {
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
    }
    cb_ = nullptr;
}

// ============================================================================ AudioToolbox decoder (AAC / ALAC)

namespace {

class AppleDecoder final : public Decoder {
public:
    explicit AppleDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}
    ~AppleDecoder() override {
        if (ext_) ExtAudioFileDispose(ext_);
        if (file_) AudioFileClose(file_);
    }

    Result<void> init() {
        if (AudioFileOpenWithCallbacks(this, &readProc, nullptr, &sizeProc, nullptr, kAudioFileM4AType, &file_) != noErr)
            return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": AudioToolbox can't open it");
        if (ExtAudioFileWrapAudioFileID(file_, false, &ext_) != noErr) return Result<void>::fail(ErrorCode::Unsupported, "ExtAudioFile");
        AudioStreamBasicDescription in{};
        UInt32 sz = sizeof in;
        ExtAudioFileGetProperty(ext_, kExtAudioFileProperty_FileDataFormat, &sz, &in);
        SInt64 frames = 0;
        sz = sizeof frames;
        ExtAudioFileGetProperty(ext_, kExtAudioFileProperty_FileLengthFrames, &sz, &frames);
        fmt_.sampleRate = static_cast<uint32_t>(in.mSampleRate);
        fmt_.channels = static_cast<uint16_t>(in.mChannelsPerFrame);
        lossless_ = in.mFormatID == kAudioFormatAppleLossless;
        // ALAC reports its real depth via the format flags; AAC decodes to float, carried as 24 significant bits.
        fmt_.bitsPerSample = lossless_ ? (in.mFormatFlags == kAppleLosslessFormatFlag_16BitSourceData ? 16
                                          : in.mFormatFlags == kAppleLosslessFormatFlag_20BitSourceData ? 24
                                          : in.mFormatFlags == kAppleLosslessFormatFlag_24BitSourceData ? 24 : 32)
                                       : 24;
        total_ = frames > 0 ? static_cast<uint64_t>(frames) : 0;
        // Client format: interleaved, left-justified int32 — exactly the engine's internal format.
        AudioStreamBasicDescription out{};
        out.mSampleRate = in.mSampleRate;
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        out.mChannelsPerFrame = in.mChannelsPerFrame;
        out.mBitsPerChannel = 32;
        out.mBytesPerFrame = 4 * in.mChannelsPerFrame;
        out.mFramesPerPacket = 1;
        out.mBytesPerPacket = out.mBytesPerFrame;
        if (ExtAudioFileSetProperty(ext_, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out) != noErr)
            return Result<void>::fail(ErrorCode::Unsupported, "client format");
        if (!fmt_.valid()) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": layout");
        return Result<void>::success();
    }

    const StreamFormat& format() const noexcept override { return fmt_; }
    uint64_t totalFrames() const noexcept override { return total_; }
    const char* codecName() const noexcept override { return lossless_ ? "ALAC" : "AAC"; }

    Result<uint32_t> read(int32_t* dst, uint32_t frames) noexcept override {
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = fmt_.channels;
        abl.mBuffers[0].mDataByteSize = frames * 4u * fmt_.channels;
        abl.mBuffers[0].mData = dst;
        UInt32 n = frames;
        if (ExtAudioFileRead(ext_, &n, &abl) != noErr) return Result<uint32_t>::fail(ioError_ ? ErrorCode::Io : ErrorCode::Corrupt, src_->name());
        return static_cast<uint32_t>(n);
    }

    Result<void> seek(uint64_t frame) noexcept override {
        if (ExtAudioFileSeek(ext_, static_cast<SInt64>(frame)) != noErr) return Result<void>::fail(ErrorCode::Corrupt, "seek");
        return Result<void>::success();
    }

private:
    static OSStatus readProc(void* user, SInt64 pos, UInt32 count, void* buf, UInt32* actual) {
        auto* d = static_cast<AppleDecoder*>(user);
        auto r = d->src_->readAt(static_cast<uint64_t>(pos), buf, count);
        if (!r) { d->ioError_ = true; *actual = 0; return kAudioFileUnspecifiedError; }
        *actual = static_cast<UInt32>(r.value());
        return noErr;
    }
    static SInt64 sizeProc(void* user) { return static_cast<SInt64>(static_cast<AppleDecoder*>(user)->src_->size()); }

    std::unique_ptr<ByteSource> src_;
    AudioFileID file_ = nullptr;
    ExtAudioFileRef ext_ = nullptr;
    StreamFormat fmt_{};
    uint64_t total_ = 0;
    bool lossless_ = false, ioError_ = false;
};

}  // namespace

void registerAppleDecoder() {
    registerPlatformDecoder([](std::unique_ptr<ByteSource>& src) -> Result<std::unique_ptr<Decoder>> {
        auto d = std::make_unique<AppleDecoder>(std::move(src));
        auto r = d->init();
        if (!r) return r.error();  // the source went with the failed decoder; openDecoder reports Unsupported
        return std::unique_ptr<Decoder>(std::move(d));
    });
}

}  // namespace groove::ios
