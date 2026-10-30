// An AudioOutput with no device: pulls audio on its own thread, optionally paced and optionally
// capturing everything it receives. Used by tests and the offline renderer (groove-cli render).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "groove/AudioOutput.h"

namespace groove {

class NullOutput final : public AudioOutput {
public:
    struct Options {
        SampleFormat format = SampleFormat::Int32;  // what the "device" accepts
        bool osResamples = false;
        uint32_t framesPerCallback = 512;
        double speed = 8.0;          // multiples of real time; 0 = as fast as possible
        bool capture = true;
        size_t captureLimitBytes = 64u << 20;
    };

    explicit NullOutput(Options o) : opts_(o) {}
    ~NullOutput() override { close(); }

    Result<OutputConfig> open(const OutputRequest& req, Callback* cb) noexcept override;
    Result<void> start() noexcept override;
    Result<void> stop() noexcept override;
    void close() noexcept override;
    const OutputConfig& config() const noexcept override { return cfg_; }

    /// Simulate the device disappearing (tests the engine's recovery path).
    void injectDeviceError() noexcept;
    /// Make the next N open() calls fail.
    void failNextOpens(int n) noexcept { failOpens_.store(n); }

    /// Everything rendered so far, plus a record of each (re)open. Thread-safe copies.
    std::vector<uint8_t> captured() const;
    std::vector<OutputConfig> openHistory() const;
    uint64_t callbacks() const noexcept { return callbacks_.load(); }

private:
    void run() noexcept;

    Options opts_;
    OutputConfig cfg_{};
    Callback* cb_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> failOpens_{0};
    std::atomic<uint64_t> callbacks_{0};
    mutable std::mutex capMutex_;
    std::vector<uint8_t> captured_;
    std::vector<OutputConfig> opens_;
    std::vector<uint8_t> block_;
};

}  // namespace groove
