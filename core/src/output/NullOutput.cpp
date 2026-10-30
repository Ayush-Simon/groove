#include "groove/NullOutput.h"

#include <chrono>
#include <new>

namespace groove {

Result<OutputConfig> NullOutput::open(const OutputRequest& req, Callback* cb) noexcept {
    close();
    int f = failOpens_.load();
    if (f > 0) {
        failOpens_.store(f - 1);
        return Result<OutputConfig>::fail(ErrorCode::DeviceOpenFailed, "simulated open failure");
    }
    if (!cb || req.sampleRate == 0 || req.channels == 0) return Result<OutputConfig>::fail(ErrorCode::InvalidArgument, "bad request");
    cfg_ = OutputConfig{};
    cfg_.sampleRate = req.sampleRate;
    cfg_.channels = req.channels;
    cfg_.format = opts_.format;
    cfg_.framesPerBurst = opts_.framesPerCallback;
    cfg_.exclusive = !opts_.osResamples;
    cfg_.osResamples = opts_.osResamples;
    cfg_.deviceName = "null";
    cb_ = cb;
    try {
        block_.assign(static_cast<size_t>(opts_.framesPerCallback) * req.channels * bytesPerSample(opts_.format), 0);
        std::lock_guard<std::mutex> lk(capMutex_);
        opens_.push_back(cfg_);
    } catch (const std::bad_alloc&) {
        return Result<OutputConfig>::fail(ErrorCode::OutOfMemory, "null output");
    }
    return cfg_;
}

Result<void> NullOutput::start() noexcept {
    if (!cb_) return Result<void>::fail(ErrorCode::InvalidState, "not open");
    if (running_.load()) return Result<void>::success();
    running_.store(true);
    try {
        thread_ = std::thread(&NullOutput::run, this);
    } catch (...) {
        running_.store(false);
        return Result<void>::fail(ErrorCode::DeviceOpenFailed, "thread");
    }
    return Result<void>::success();
}

Result<void> NullOutput::stop() noexcept {
    running_.store(false);
    if (thread_.joinable()) thread_.join();  // synchronous, like AAudio requestStop + wait
    return Result<void>::success();
}

void NullOutput::close() noexcept {
    (void)stop();
    cb_ = nullptr;
}

void NullOutput::injectDeviceError() noexcept {
    if (cb_) cb_->onDeviceError(ErrorCode::DeviceLost);
}

void NullOutput::run() noexcept {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    uint64_t frames = 0;
    while (running_.load()) {
        cb_->onRender(block_.data(), opts_.framesPerCallback);
        callbacks_.fetch_add(1);
        frames += opts_.framesPerCallback;
        if (opts_.capture) {
            try {
                std::lock_guard<std::mutex> lk(capMutex_);
                if (captured_.size() + block_.size() <= opts_.captureLimitBytes) captured_.insert(captured_.end(), block_.begin(), block_.end());
            } catch (...) {
            }
        }
        if (opts_.speed > 0) {
            const double secs = static_cast<double>(frames) / cfg_.sampleRate / opts_.speed;
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs)));
        }
    }
}

std::vector<uint8_t> NullOutput::captured() const {
    std::lock_guard<std::mutex> lk(capMutex_);
    return captured_;
}

std::vector<OutputConfig> NullOutput::openHistory() const {
    std::lock_guard<std::mutex> lk(capMutex_);
    return opens_;
}

}  // namespace groove
