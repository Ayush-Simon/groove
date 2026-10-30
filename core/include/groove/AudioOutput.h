// Audio output abstraction. Implementations live with the platform:
//   platform/android/jni/AAudioOutput.*   (AAudio, Android 8.0+)
//   platform/ios/GrooveCore/CoreAudioOutput.*  (RemoteIO + AVAudioSession)
//   core/src/output/NullOutput.*           (tests / offline rendering)
#pragma once

#include <cstdint>
#include <string>

#include "groove/Result.h"
#include "groove/Types.h"

namespace groove {

/// What the engine asks for: the source's own rate/channels and the best sample format for it.
struct OutputRequest {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t sourceBits = 16;
    bool preferExclusive = true;  // ask for a path that skips the system mixer when the OS allows it
};

/// What the device actually granted.
struct OutputConfig {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    SampleFormat format = SampleFormat::Float32;
    uint32_t framesPerBurst = 0;
    bool exclusive = false;         // OS reports a mixer-bypassing path (AAudio EXCLUSIVE/MMAP, USB bit-perfect mixer)
    bool osResamples = true;        // the OS will convert the rate before the DAC
    std::string deviceName;
};

class AudioOutput {
public:
    class Callback {
    public:
        virtual ~Callback() = default;
        /// Real-time thread. Must fill `frames` frames in config().format. No locks, no allocation.
        virtual void onRender(void* dst, uint32_t frames) noexcept = 0;
        /// Any thread (often the real-time one). The engine only sets a flag and reacts elsewhere.
        virtual void onDeviceError(ErrorCode code) noexcept = 0;
    };

    virtual ~AudioOutput() = default;
    virtual Result<OutputConfig> open(const OutputRequest& req, Callback* cb) noexcept = 0;
    /// start/stop are synchronous: after stop() returns, onRender is not running and won't be called.
    virtual Result<void> start() noexcept = 0;
    virtual Result<void> stop() noexcept = 0;
    virtual void close() noexcept = 0;
    virtual const OutputConfig& config() const noexcept = 0;
};

}  // namespace groove
