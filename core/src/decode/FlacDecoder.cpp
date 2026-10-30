// FLAC via libFLAC's stream decoder, reading through ByteSource (no mmap; see ByteSource.h).
#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "groove/Decoder.h"

namespace groove {
namespace {

struct FlacDeleter {
    void operator()(FLAC__StreamDecoder* d) const noexcept {
        if (d) {
            FLAC__stream_decoder_finish(d);
            FLAC__stream_decoder_delete(d);
        }
    }
};

class FlacDecoder final : public Decoder {
public:
    explicit FlacDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}

    Result<void> init() noexcept {
        dec_.reset(FLAC__stream_decoder_new());
        if (!dec_) return Result<void>::fail(ErrorCode::OutOfMemory, "FLAC decoder");
        FLAC__stream_decoder_set_md5_checking(dec_.get(), false);
        const auto st = FLAC__stream_decoder_init_stream(dec_.get(), &readCb, &seekCb, &tellCb, &lengthCb, &eofCb, &writeCb,
                                                         &metadataCb, &errorCb, this);
        if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": FLAC init failed");
        if (!FLAC__stream_decoder_process_until_end_of_metadata(dec_.get()) || ioError_)
            return Result<void>::fail(ioError_ ? ErrorCode::Io : ErrorCode::Corrupt, src_->name() + ": bad FLAC header");
        if (!fmt_.valid()) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": no STREAMINFO");
        return Result<void>::success();
    }

    const StreamFormat& format() const noexcept override { return fmt_; }
    uint64_t totalFrames() const noexcept override { return total_; }
    const char* codecName() const noexcept override { return "FLAC"; }

    Result<uint32_t> read(int32_t* dst, uint32_t frames) noexcept override {
        const uint16_t ch = fmt_.channels;
        uint32_t done = 0;
        while (done < frames) {
            if (blockPos_ < blockFrames_) {
                const uint32_t n = std::min<uint32_t>(frames - done, static_cast<uint32_t>(blockFrames_ - blockPos_));
                std::memcpy(dst + static_cast<size_t>(done) * ch, block_.data() + blockPos_ * ch, static_cast<size_t>(n) * ch * sizeof(int32_t));
                blockPos_ += n;
                done += n;
                continue;
            }
            if (eof_) break;
            blockFrames_ = blockPos_ = 0;
            const bool ok = FLAC__stream_decoder_process_single(dec_.get());
            if (ioError_) return Result<uint32_t>::fail(ErrorCode::Io, src_->name() + ": read error (storage removed?)");
            if (oom_) return Result<uint32_t>::fail(ErrorCode::OutOfMemory, "FLAC block");
            const auto state = FLAC__stream_decoder_get_state(dec_.get());
            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) eof_ = true;
            else if (!ok || state == FLAC__STREAM_DECODER_ABORTED || state == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR)
                return Result<uint32_t>::fail(ErrorCode::Corrupt, src_->name() + ": FLAC decode error");
        }
        return done;
    }

    Result<void> seek(uint64_t frame) noexcept override {
        if (total_ > 0 && frame >= total_) frame = total_ - 1;
        blockFrames_ = blockPos_ = 0;
        eof_ = false;
        // libFLAC delivers the frame containing `frame` to the write callback trimmed so that it starts exactly there.
        if (!FLAC__stream_decoder_seek_absolute(dec_.get(), frame)) {
            if (FLAC__stream_decoder_get_state(dec_.get()) == FLAC__STREAM_DECODER_SEEK_ERROR) FLAC__stream_decoder_flush(dec_.get());
            return Result<void>::fail(ioError_ ? ErrorCode::Io : ErrorCode::Corrupt, src_->name() + ": seek failed");
        }
        return Result<void>::success();
    }

private:
    static FlacDecoder* self(void* c) { return static_cast<FlacDecoder*>(c); }

    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder*, FLAC__byte buf[], size_t* bytes, void* c) {
        FlacDecoder* d = self(c);
        if (*bytes == 0) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        auto r = d->src_->readAt(d->pos_, buf, *bytes);
        if (!r) {
            d->ioError_ = true;
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        *bytes = r.value();
        d->pos_ += r.value();
        return r.value() == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    static FLAC__StreamDecoderSeekStatus seekCb(const FLAC__StreamDecoder*, FLAC__uint64 off, void* c) {
        FlacDecoder* d = self(c);
        if (off > d->src_->size()) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        d->pos_ = off;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }
    static FLAC__StreamDecoderTellStatus tellCb(const FLAC__StreamDecoder*, FLAC__uint64* off, void* c) {
        *off = self(c)->pos_;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    static FLAC__StreamDecoderLengthStatus lengthCb(const FLAC__StreamDecoder*, FLAC__uint64* len, void* c) {
        *len = self(c)->src_->size();
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }
    static FLAC__bool eofCb(const FLAC__StreamDecoder*, void* c) {
        FlacDecoder* d = self(c);
        return d->pos_ >= d->src_->size();
    }
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const buf[], void* c) {
        FlacDecoder* d = self(c);
        const unsigned ch = f->header.channels, n = f->header.blocksize, bps = f->header.bits_per_sample;
        if (ch != d->fmt_.channels || bps == 0 || bps > 32) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;  // mid-stream layout change
        try {
            if (d->block_.size() < static_cast<size_t>(n) * ch) d->block_.resize(static_cast<size_t>(n) * ch);
        } catch (...) {
            d->oom_ = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const unsigned shift = 32 - bps;
        int32_t* out = d->block_.data();
        for (unsigned i = 0; i < n; ++i)
            for (unsigned k = 0; k < ch; ++k)
                *out++ = static_cast<int32_t>(static_cast<uint32_t>(buf[k][i]) << shift);  // left-justify, exact
        d->blockFrames_ = n;
        d->blockPos_ = 0;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    static void metadataCb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* c) {
        if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        FlacDecoder* d = self(c);
        d->fmt_.sampleRate = m->data.stream_info.sample_rate;
        d->fmt_.channels = static_cast<uint16_t>(m->data.stream_info.channels);
        d->fmt_.bitsPerSample = static_cast<uint16_t>(m->data.stream_info.bits_per_sample);
        d->total_ = m->data.stream_info.total_samples;
    }
    static void errorCb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {
        // Lost sync / bad CRC: libFLAC resynchronises and skips the frame. Not fatal.
    }

    std::unique_ptr<ByteSource> src_;
    std::unique_ptr<FLAC__StreamDecoder, FlacDeleter> dec_;
    uint64_t pos_ = 0;
    StreamFormat fmt_{};
    uint64_t total_ = 0;
    std::vector<int32_t> block_;
    size_t blockFrames_ = 0, blockPos_ = 0;
    bool eof_ = false, ioError_ = false, oom_ = false;
};

}  // namespace

Result<std::unique_ptr<Decoder>> openFlacDecoder(std::unique_ptr<ByteSource> src) noexcept {
    using R = Result<std::unique_ptr<Decoder>>;
    if (!src) return R::fail(ErrorCode::InvalidArgument, "no source");
    try {
        auto d = std::make_unique<FlacDecoder>(std::move(src));
        auto r = d->init();
        if (!r) return r.error();
        return R(std::unique_ptr<Decoder>(std::move(d)));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "FLAC decoder");
    }
}

}  // namespace groove
