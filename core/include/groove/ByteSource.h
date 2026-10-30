// Scoped file access for decoders and the metadata parser.
//
// Why pread() and not mmap(): a memory-mapped file on removable storage (SD card, USB drive) turns
// "the drive was unplugged" into SIGBUS, which can't be recovered from safely. pread() turns the same
// event into an ordinary error code that the engine handles (skip to the next track / Error state).
// All reads happen on the decoder or scanner thread — never on the real-time audio thread, which only
// reads from the ring buffer — so blocking I/O can't cause an underrun as long as the ring has data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groove/Result.h"

namespace groove {

/// Owns a POSIX file descriptor; closes it exactly once.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept { int f = fd_; fd_ = -1; return f; }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/// Random-access byte reader.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const noexcept = 0;
    /// Reads up to `len` bytes at `offset`. Returns the count (0 at end of file) or an Io error.
    virtual Result<size_t> readAt(uint64_t offset, void* dst, size_t len) noexcept = 0;
    virtual const std::string& name() const noexcept = 0;
};

/// File-descriptor backed source with a small read-ahead cache for sequential decoding.
class FileSource final : public ByteSource {
public:
    static Result<std::unique_ptr<FileSource>> openPath(const std::string& path) noexcept;
    /// Takes ownership of an already-open descriptor (Android: ParcelFileDescriptor.detachFd()).
    static Result<std::unique_ptr<FileSource>> adopt(UniqueFd fd, std::string name) noexcept;

    uint64_t size() const noexcept override { return size_; }
    Result<size_t> readAt(uint64_t offset, void* dst, size_t len) noexcept override;
    const std::string& name() const noexcept override { return name_; }

private:
    struct PrivateTag { explicit PrivateTag() = default; };  // only FileSource can name this type

public:
    FileSource(PrivateTag, UniqueFd fd, uint64_t size, std::string name);

private:
    UniqueFd fd_;
    uint64_t size_;
    std::string name_;
    std::vector<uint8_t> cache_;  // read-ahead window
    uint64_t cacheOffset_ = 0;
    size_t cacheLen_ = 0;
    static constexpr size_t kCacheSize = 256 * 1024;
};

/// In-memory source (tests, fuzzing, embedded artwork).
class MemorySource final : public ByteSource {
public:
    MemorySource(std::vector<uint8_t> bytes, std::string name) : bytes_(std::move(bytes)), name_(std::move(name)) {}
    uint64_t size() const noexcept override { return bytes_.size(); }
    Result<size_t> readAt(uint64_t offset, void* dst, size_t len) noexcept override;
    const std::string& name() const noexcept override { return name_; }

private:
    std::vector<uint8_t> bytes_;
    std::string name_;
};

/// How the engine/scanner turns a TrackSource::uri into bytes. Desktop/iOS: open the path.
/// Android: the JNI layer implements this by asking Kotlin for a ParcelFileDescriptor (SAF).
class SourceOpener {
public:
    virtual ~SourceOpener() = default;
    virtual Result<std::unique_ptr<ByteSource>> open(const std::string& uri) noexcept = 0;
};

class PathOpener final : public SourceOpener {
public:
    Result<std::unique_ptr<ByteSource>> open(const std::string& uri) noexcept override;
};

}  // namespace groove
