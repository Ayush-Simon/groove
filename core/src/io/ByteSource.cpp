#include "groove/ByteSource.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

namespace groove {

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
        // close() must not be retried on EINTR on Linux/Android/Darwin: the fd is already released.
        ::close(fd_);
    }
    fd_ = fd;
}

FileSource::FileSource(PrivateTag, UniqueFd fd, uint64_t size, std::string name)
    : fd_(std::move(fd)), size_(size), name_(std::move(name)) {}

Result<std::unique_ptr<FileSource>> FileSource::openPath(const std::string& path) noexcept {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int raw;
    do { raw = ::open(path.c_str(), flags); } while (raw < 0 && errno == EINTR);
    if (raw < 0) {
        const int e = errno;
        return Result<std::unique_ptr<FileSource>>::fail(e == ENOENT || e == EACCES || e == EPERM ? ErrorCode::FileNotFound : ErrorCode::Io,
                                                         path + ": " + std::strerror(e));
    }
    return adopt(UniqueFd(raw), path);
}

Result<std::unique_ptr<FileSource>> FileSource::adopt(UniqueFd fd, std::string name) noexcept {
    using R = Result<std::unique_ptr<FileSource>>;
    if (!fd.valid()) return R::fail(ErrorCode::InvalidArgument, "invalid file descriptor");
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) return R::fail(ErrorCode::Io, name + ": fstat failed");
    if (!S_ISREG(st.st_mode)) return R::fail(ErrorCode::Unsupported, name + ": not a regular file");
    try {
        auto src = std::make_unique<FileSource>(PrivateTag{}, std::move(fd), static_cast<uint64_t>(st.st_size), std::move(name));
        src->cache_.resize(kCacheSize);
        return R(std::move(src));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "file cache");
    }
}

Result<size_t> FileSource::readAt(uint64_t offset, void* dst, size_t len) noexcept {
    if (offset >= size_ || len == 0) return size_t{0};
    if (len > size_ - offset) len = static_cast<size_t>(size_ - offset);
    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        const uint64_t pos = offset + done;
        // Serve from the read-ahead cache when possible.
        if (pos >= cacheOffset_ && pos < cacheOffset_ + cacheLen_) {
            const size_t at = static_cast<size_t>(pos - cacheOffset_);
            const size_t n = std::min(len - done, cacheLen_ - at);
            std::memcpy(out + done, cache_.data() + at, n);
            done += n;
            continue;
        }
        // Large reads bypass the cache; small ones refill it.
        const bool direct = (len - done) >= kCacheSize;
        uint8_t* target = direct ? out + done : cache_.data();
        const size_t want = direct ? (len - done) : kCacheSize;
        ssize_t got;
        do { got = ::pread(fd_.get(), target, want, static_cast<off_t>(pos)); } while (got < 0 && errno == EINTR);
        if (got < 0) return Result<size_t>::fail(ErrorCode::Io, name_ + ": read failed: " + std::strerror(errno));
        if (got == 0) break;  // file shrank underneath us
        if (direct) {
            done += static_cast<size_t>(got);
        } else {
            cacheOffset_ = pos;
            cacheLen_ = static_cast<size_t>(got);
        }
    }
    return done;
}

Result<size_t> MemorySource::readAt(uint64_t offset, void* dst, size_t len) noexcept {
    if (offset >= bytes_.size() || len == 0) return size_t{0};
    const size_t n = std::min(len, static_cast<size_t>(bytes_.size() - offset));
    std::memcpy(dst, bytes_.data() + offset, n);
    return n;
}

Result<std::unique_ptr<ByteSource>> PathOpener::open(const std::string& uri) noexcept {
    auto r = FileSource::openPath(uri);
    if (!r) return r.error();
    return std::unique_ptr<ByteSource>(std::move(r).value());
}

}  // namespace groove
