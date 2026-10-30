// AAudio implementation of groove::AudioOutput (Android 8.0 / API 26+).
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <memory>

#include "groove/AudioOutput.h"

namespace groove::android {

struct DeviceInfo {
    int32_t apiLevel = 26;
    uint32_t mixerSampleRate = 48000;  // AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE
    bool bitPerfectMixer = false;      // Android 14+: USB device routed with MIXER_BEHAVIOR_BIT_PERFECT
};

class AAudioOutput final : public AudioOutput {
public:
    explicit AAudioOutput(DeviceInfo info)
        : apiLevel_(info.apiLevel), mixerRate_(info.mixerSampleRate), bitPerfectMixer_(info.bitPerfectMixer) {}
    ~AAudioOutput() override { close(); }

    Result<OutputConfig> open(const OutputRequest& req, Callback* cb) noexcept override;
    Result<void> start() noexcept override;
    Result<void> stop() noexcept override;
    void close() noexcept override;
    const OutputConfig& config() const noexcept override { return cfg_; }

    /// Kotlin updates this when the user plugs in / unplugs a USB DAC (BitPerfectRouting.kt). Any thread;
    /// takes effect at the next open (the engine reopens on route changes).
    void setDeviceInfo(uint32_t mixerSampleRate, bool bitPerfectMixer) noexcept {
        mixerRate_.store(mixerSampleRate);
        bitPerfectMixer_.store(bitPerfectMixer);
    }

private:
    struct StreamCloser {
        void operator()(AAudioStream* s) const noexcept { if (s) AAudioStream_close(s); }
    };
    struct BuilderDeleter {
        void operator()(AAudioStreamBuilder* b) const noexcept { if (b) AAudioStreamBuilder_delete(b); }
    };

    Result<OutputConfig> tryOpen(const OutputRequest& req, aaudio_format_t fmt, bool exclusive) noexcept;
    static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* user, void* audio, int32_t frames);
    static void errorCallback(AAudioStream*, void* user, aaudio_result_t error);

    const int32_t apiLevel_;
    std::atomic<uint32_t> mixerRate_;
    std::atomic<bool> bitPerfectMixer_;
    std::unique_ptr<AAudioStream, StreamCloser> stream_;
    Callback* cb_ = nullptr;
    OutputConfig cfg_{};
};

}  // namespace groove::android
