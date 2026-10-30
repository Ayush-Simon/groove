// Lock-free single-producer / single-consumer ring buffer.
//
// The decoder thread is the only producer, the real-time audio callback is the only consumer.
// No locks, no allocation and no system calls after construction, so the consumer side is safe to
// call from a real-time thread. Capacity is rounded up to a power of two.
//
// reset() is NOT thread-safe: the engine only calls it while both the decoder and the output
// stream are stopped (see AudioEngine::stopTheWorld).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace groove {

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable<T>::value, "SpscRing holds trivially copyable items only");

public:
    explicit SpscRing(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        buf_ = std::make_unique<T[]>(cap);  // value-initialised (zeroed)
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t capacity() const noexcept { return capacity_; }

    /// Producer: number of items that can be written right now.
    size_t writable() const noexcept {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        return capacity_ - static_cast<size_t>(w - r);
    }

    /// Consumer: number of items that can be read right now.
    size_t readable() const noexcept {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    /// Producer: copies up to n items, returns how many were written.
    size_t write(const T* src, size_t n) noexcept {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t space = capacity_ - static_cast<size_t>(w - r);
        n = std::min(n, space);
        if (n == 0) return 0;
        const size_t start = static_cast<size_t>(w) & mask_;
        const size_t first = std::min(n, capacity_ - start);
        std::memcpy(buf_.get() + start, src, first * sizeof(T));
        if (n > first) std::memcpy(buf_.get(), src + first, (n - first) * sizeof(T));
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    /// Consumer: copies up to n items into dst, returns how many were read.
    size_t read(T* dst, size_t n) noexcept {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(w - r));
        if (n == 0) return 0;
        const size_t start = static_cast<size_t>(r) & mask_;
        const size_t first = std::min(n, capacity_ - start);
        std::memcpy(dst, buf_.get() + start, first * sizeof(T));
        if (n > first) std::memcpy(dst + first, buf_.get(), (n - first) * sizeof(T));
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    /// Consumer: drops up to n items.
    size_t skip(size_t n) noexcept {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const uint64_t w = write_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(w - r));
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    /// Total items ever written / read (monotonic; used to place track markers).
    uint64_t totalWritten() const noexcept { return write_.load(std::memory_order_acquire); }
    uint64_t totalRead() const noexcept { return read_.load(std::memory_order_acquire); }

    /// Only while neither side is running.
    void reset() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

private:
    std::unique_ptr<T[]> buf_;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    // Separate cache lines so producer and consumer don't false-share.
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

}  // namespace groove
