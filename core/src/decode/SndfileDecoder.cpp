// WAV / AIFF / W64 / CAF / Ogg Vorbis / Opus / MP3 (libsndfile ≥ 1.1) through sf_open_virtual.
// Licensing: libsndfile is LGPL-2.1 — link it dynamically (Android .so is fine; on iOS ship it as a
// dynamic framework or swap this decoder for AudioToolbox). See docs/ENGINE.md.
#include <sndfile.h>

#include <algorithm>
#include <new>

#include "groove/Decoder.h"

namespace groove {
namespace {

struct SndfileCloser {
    void operator()(SNDFILE* f) const noexcept {
        if (f) sf_close(f);
    }
};

class SndfileDecoder final : public Decoder {
public:
    explicit SndfileDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}

    Result<void> init() noexcept {
        vio_.get_filelen = [](void* u) -> sf_count_t { return static_cast<sf_count_t>(self(u)->src_->size()); };
        vio_.seek = [](sf_count_t off, int whence, void* u) -> sf_count_t {
            auto* d = self(u);
            sf_count_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<sf_count_t>(d->pos_) : static_cast<sf_count_t>(d->src_->size());
            const sf_count_t np = base + off;
            if (np < 0) return -1;
            d->pos_ = static_cast<uint64_t>(np);
            return np;
        };
        vio_.read = [](void* buf, sf_count_t n, void* u) -> sf_count_t {
            auto* d = self(u);
            if (n <= 0) return 0;
            auto r = d->src_->readAt(d->pos_, buf, static_cast<size_t>(n));
            if (!r) {
                d->ioError_ = true;
                return 0;
            }
            d->pos_ += r.value();
            return static_cast<sf_count_t>(r.value());
        };
        vio_.write = [](const void*, sf_count_t, void*) -> sf_count_t { return 0; };
        vio_.tell = [](void* u) -> sf_count_t { return static_cast<sf_count_t>(self(u)->pos_); };

        SF_INFO info{};
        file_.reset(sf_open_virtual(&vio_, SFM_READ, &info, this));
        if (!file_) {
            return Result<void>::fail(ioError_ ? ErrorCode::Io : ErrorCode::Unsupported,
                                      src_->name() + ": " + sf_strerror(nullptr));
        }
        // Floating-point sources are scaled to the full int range instead of being truncated.
        sf_command(file_.get(), SFC_SET_SCALE_FLOAT_INT_READ, nullptr, SF_TRUE);
        fmt_.sampleRate = static_cast<uint32_t>(info.samplerate);
        fmt_.channels = static_cast<uint16_t>(info.channels);
        total_ = info.frames > 0 ? static_cast<uint64_t>(info.frames) : 0;
        seekable_ = info.seekable != 0;
        switch (info.format & SF_FORMAT_SUBMASK) {
            case SF_FORMAT_PCM_S8: case SF_FORMAT_PCM_U8: fmt_.bitsPerSample = 8; codec_ = "PCM"; break;
            case SF_FORMAT_PCM_16: fmt_.bitsPerSample = 16; codec_ = "PCM"; break;
            case SF_FORMAT_PCM_24: fmt_.bitsPerSample = 24; codec_ = "PCM"; break;
            case SF_FORMAT_PCM_32: fmt_.bitsPerSample = 32; codec_ = "PCM"; break;
            case SF_FORMAT_FLOAT: fmt_.bitsPerSample = 32; codec_ = "PCM float"; break;
            case SF_FORMAT_DOUBLE: fmt_.bitsPerSample = 32; codec_ = "PCM double"; break;
            case SF_FORMAT_VORBIS: fmt_.bitsPerSample = 24; codec_ = "Vorbis"; break;
            case SF_FORMAT_OPUS: fmt_.bitsPerSample = 24; codec_ = "Opus"; break;
            case SF_FORMAT_FLAC: fmt_.bitsPerSample = 24; codec_ = "FLAC"; break;
            default: fmt_.bitsPerSample = 24; codec_ = "audio"; break;  // MPEG etc.: decoded to float internally
        }
        if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_MPEG) codec_ = "MP3";
        if (!fmt_.valid()) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": unsupported layout");
        return Result<void>::success();
    }

    const StreamFormat& format() const noexcept override { return fmt_; }
    uint64_t totalFrames() const noexcept override { return total_; }
    const char* codecName() const noexcept override { return codec_; }

    Result<uint32_t> read(int32_t* dst, uint32_t frames) noexcept override {
        // sf_readf_int returns samples scaled to the full 32-bit range: exactly our left-justified format.
        const sf_count_t got = sf_readf_int(file_.get(), dst, frames);
        if (ioError_) return Result<uint32_t>::fail(ErrorCode::Io, src_->name() + ": read error (storage removed?)");
        if (got < 0) return Result<uint32_t>::fail(ErrorCode::Corrupt, src_->name() + ": decode error");
        return static_cast<uint32_t>(got);
    }

    Result<void> seek(uint64_t frame) noexcept override {
        if (!seekable_) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": not seekable");
        if (sf_seek(file_.get(), static_cast<sf_count_t>(frame), SEEK_SET) < 0)
            return Result<void>::fail(ioError_ ? ErrorCode::Io : ErrorCode::Corrupt, src_->name() + ": seek failed");
        return Result<void>::success();
    }

private:
    static SndfileDecoder* self(void* u) { return static_cast<SndfileDecoder*>(u); }

    std::unique_ptr<ByteSource> src_;
    SF_VIRTUAL_IO vio_{};
    std::unique_ptr<SNDFILE, SndfileCloser> file_;
    uint64_t pos_ = 0;
    StreamFormat fmt_{};
    uint64_t total_ = 0;
    bool seekable_ = false, ioError_ = false;
    const char* codec_ = "audio";
};

}  // namespace

Result<std::unique_ptr<Decoder>> openSndfileDecoder(std::unique_ptr<ByteSource> src) noexcept {
    using R = Result<std::unique_ptr<Decoder>>;
    if (!src) return R::fail(ErrorCode::InvalidArgument, "no source");
    try {
        auto d = std::make_unique<SndfileDecoder>(std::move(src));
        auto r = d->init();
        if (!r) return r.error();
        return R(std::unique_ptr<Decoder>(std::move(d)));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "sndfile decoder");
    }
}

}  // namespace groove
