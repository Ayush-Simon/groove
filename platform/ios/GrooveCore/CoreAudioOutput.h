// RemoteIO (AUAudioUnit-free, plain AudioUnit C API) implementation of groove::AudioOutput.
#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include "groove/AudioOutput.h"

namespace groove::ios {

class CoreAudioOutput final : public AudioOutput {
public:
    CoreAudioOutput() = default;
    ~CoreAudioOutput() override { close(); }

    Result<OutputConfig> open(const OutputRequest& req, Callback* cb) noexcept override;
    Result<void> start() noexcept override;
    Result<void> stop() noexcept override;
    void close() noexcept override;
    const OutputConfig& config() const noexcept override { return cfg_; }

    /// Called by the Obj-C++ layer on AVAudioSession route changes / media-services resets.
    void notifyRouteLost() noexcept { if (cb_) cb_->onDeviceError(ErrorCode::DeviceLost); }

private:
    static OSStatus render(void* user, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                           AudioBufferList* io);
    AudioComponentInstance unit_ = nullptr;
    Callback* cb_ = nullptr;
    OutputConfig cfg_{};
    bool running_ = false;
};

/// AAC / ALAC / Apple Lossless in MP4 through AudioToolbox (ExtAudioFile over our ByteSource).
void registerAppleDecoder();

}  // namespace groove::ios
