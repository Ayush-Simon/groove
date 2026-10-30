#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <new>

#include "groove/Metadata.h"

namespace groove {
namespace {

// ---------------------------------------------------------------- bounded byte access
struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool has(size_t off, size_t len) const noexcept { return off <= n && len <= n - off; }
    uint8_t u8(size_t o) const noexcept { return has(o, 1) ? p[o] : 0; }
    uint16_t be16(size_t o) const noexcept { return has(o, 2) ? static_cast<uint16_t>(p[o] << 8 | p[o + 1]) : 0; }
    uint16_t le16(size_t o) const noexcept { return has(o, 2) ? static_cast<uint16_t>(p[o] | p[o + 1] << 8) : 0; }
    uint32_t be24(size_t o) const noexcept { return has(o, 3) ? (uint32_t(p[o]) << 16 | p[o + 1] << 8 | p[o + 2]) : 0; }
    uint32_t be32(size_t o) const noexcept { return has(o, 4) ? (uint32_t(p[o]) << 24 | p[o + 1] << 16 | p[o + 2] << 8 | p[o + 3]) : 0; }
    uint32_t le32(size_t o) const noexcept { return has(o, 4) ? (p[o] | p[o + 1] << 8 | p[o + 2] << 16 | uint32_t(p[o + 3]) << 24) : 0; }
    uint64_t be64(size_t o) const noexcept { return uint64_t(be32(o)) << 32 | be32(o + 4); }
    uint64_t le64(size_t o) const noexcept { return le32(o) | uint64_t(le32(o + 4)) << 32; }
    uint32_t syncsafe(size_t o) const noexcept {
        return has(o, 4) ? ((p[o] & 0x7fu) << 21 | (p[o + 1] & 0x7fu) << 14 | (p[o + 2] & 0x7fu) << 7 | (p[o + 3] & 0x7fu)) : 0;
    }
    bool is(size_t o, const char* tag) const noexcept {
        const size_t len = std::strlen(tag);
        return has(o, len) && std::memcmp(p + o, tag, len) == 0;
    }
    Bytes sub(size_t off, size_t len) const noexcept {
        if (off > n) return {};
        return Bytes{p + off, std::min(len, n - off)};
    }
};

Result<std::vector<uint8_t>> readRange(ByteSource& src, uint64_t off, size_t len) {
    std::vector<uint8_t> v;
    if (off >= src.size()) return v;
    len = static_cast<size_t>(std::min<uint64_t>(len, src.size() - off));
    v.resize(len);
    auto r = src.readAt(off, v.data(), len);
    if (!r) return r.error();
    v.resize(r.value());
    return v;
}

// ---------------------------------------------------------------- text helpers
void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += char(0xC0 | cp >> 6); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | cp >> 12); s += char(0x80 | (cp >> 6 & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x110000) { s += char(0xF0 | cp >> 18); s += char(0x80 | (cp >> 12 & 0x3F)); s += char(0x80 | (cp >> 6 & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

std::string latin1(Bytes b) {
    std::string s;
    for (size_t i = 0; i < b.n && b.p[i]; ++i) appendUtf8(s, b.p[i]);
    return s;
}

std::string utf16(Bytes b, bool bigEndian) {
    std::string s;
    size_t i = 0;
    if (b.n >= 2 && ((b.p[0] == 0xFF && b.p[1] == 0xFE) || (b.p[0] == 0xFE && b.p[1] == 0xFF))) {
        bigEndian = b.p[0] == 0xFE;
        i = 2;
    }
    for (; i + 1 < b.n; i += 2) {
        uint32_t u = bigEndian ? b.be16(i) : b.le16(i);
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.n) {
            const uint32_t lo = bigEndian ? b.be16(i + 2) : b.le16(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else u = 0xFFFD;
        } else if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
        appendUtf8(s, u);
    }
    return s;
}

/// Keeps valid UTF-8, replaces invalid sequences with U+FFFD, stops at NUL.
std::string utf8(Bytes b) {
    std::string s;
    for (size_t i = 0; i < b.n && b.p[i];) {
        const uint8_t c = b.p[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && b.has(i, len);
        for (int k = 1; ok && k < len; ++k) ok = (b.p[i + k] & 0xC0) == 0x80;
        if (ok) { s.append(reinterpret_cast<const char*>(b.p + i), len); i += len; }
        else { appendUtf8(s, 0xFFFD); ++i; }
    }
    return s;
}

std::string trim(std::string s) {
    const auto ws = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    return s.substr(i);
}

int parseYear(const std::string& s) {
    for (size_t i = 0; i + 4 <= s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) && std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            const int y = std::stoi(s.substr(i, 4));
            return (y >= 1000 && y <= 2999) ? y : 0;
        }
    }
    return 0;
}

void parseNumberPair(const std::string& s, int& a, int& b) {
    int x = 0, y = 0;
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && x < 100000; ++i) x = x * 10 + (s[i] - '0');
    if (i < s.size() && s[i] == '/') {
        for (++i; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && y < 100000; ++i) y = y * 10 + (s[i] - '0');
    }
    if (x && !a) a = x;
    if (y && !b) b = y;
}

const char* const kId3Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
    "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
    "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
    "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
    "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock"};

std::string genreName(std::string g) {
    // "(13)", "13", "(13)Pop" → "Pop"
    std::string digits;
    size_t i = 0;
    if (!g.empty() && g[0] == '(') i = 1;
    while (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i])) && digits.size() < 4) digits += g[i++];
    if (!digits.empty() && (i == g.size() || (g[0] == '(' && i < g.size() && g[i] == ')'))) {
        const std::string rest = (g[0] == '(' && i + 1 < g.size()) ? g.substr(i + 1) : std::string();
        if (!rest.empty()) return rest;
        const int n = std::stoi(digits);
        if (n >= 0 && n < static_cast<int>(sizeof kId3Genres / sizeof *kId3Genres)) return kId3Genres[n];
    }
    return g;
}

void setIfEmpty(std::string& field, const std::string& v) {
    if (field.empty() && !v.empty()) field = v;
}

// ---------------------------------------------------------------- Vorbis comments (FLAC, Ogg)
void applyVorbisKey(TrackMetadata& m, std::string key, const std::string& v) {
    for (auto& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (key == "TITLE") setIfEmpty(m.title, v);
    else if (key == "ARTIST") setIfEmpty(m.artist, v);
    else if (key == "ALBUM") setIfEmpty(m.album, v);
    else if (key == "ALBUMARTIST" || key == "ALBUM ARTIST" || key == "ALBUM_ARTIST") setIfEmpty(m.albumArtist, v);
    else if (key == "GENRE") setIfEmpty(m.genre, v);
    else if (key == "COMPOSER") setIfEmpty(m.composer, v);
    else if (key == "DATE" || key == "YEAR" || key == "ORIGINALDATE") { if (!m.year) m.year = parseYear(v); }
    else if (key == "TRACKNUMBER") parseNumberPair(v, m.trackNo, m.trackTotal);
    else if (key == "TRACKTOTAL" || key == "TOTALTRACKS") { int a = 0; parseNumberPair(v, m.trackTotal, a); }
    else if (key == "DISCNUMBER") parseNumberPair(v, m.discNo, m.discTotal);
    else if (key == "DISCTOTAL" || key == "TOTALDISCS") { int a = 0; parseNumberPair(v, m.discTotal, a); }
    else if (m.extra.size() < 64) m.extra.emplace_back(key, v);
}

void parseFlacPicture(Bytes b, uint64_t fileOffset, bool addressable, TrackMetadata& m, const ParseOptions& o);
std::vector<uint8_t> base64Decode(const std::string& s);

void parseVorbisComments(Bytes b, TrackMetadata& m, const ParseOptions& o) {
    size_t p = 0;
    const uint32_t vendorLen = b.le32(p);
    p += 4;
    if (!b.has(p, vendorLen)) return;
    p += vendorLen;
    const uint32_t count = b.le32(p);
    p += 4;
    for (uint32_t i = 0; i < count && i < 4096; ++i) {
        const uint32_t len = b.le32(p);
        p += 4;
        if (!b.has(p, len)) return;
        const std::string kv = utf8(b.sub(p, len));
        p += len;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = kv.substr(0, eq);
        if (key == "METADATA_BLOCK_PICTURE" || key == "metadata_block_picture") {
            if (!m.artwork) {
                const std::vector<uint8_t> pic = base64Decode(kv.substr(eq + 1));
                parseFlacPicture(Bytes{pic.data(), pic.size()}, 0, false, m, o);
            }
            continue;
        }
        applyVorbisKey(m, key, trim(kv.substr(eq + 1)));
    }
}

std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else continue;  // '=', whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

void setArtwork(TrackMetadata& m, std::string mime, uint8_t type, Bytes data, uint64_t fileOffset, bool addressable, const ParseOptions& o) {
    if (data.n == 0) return;
    if (m.artwork && !(type == 3 && m.artwork->pictureType != 3)) return;  // prefer the front cover
    ArtworkRef a;
    a.mime = mime.empty() ? "image/jpeg" : mime;
    if (a.mime == "JPG" || a.mime == "jpg") a.mime = "image/jpeg";
    if (a.mime == "PNG" || a.mime == "png") a.mime = "image/png";
    a.pictureType = type;
    a.size = data.n;
    a.offset = addressable ? fileOffset : 0;
    a.inlineOnly = !addressable;
    if ((o.loadArtwork || !addressable) && data.n <= o.maxArtworkBytes) a.data.assign(data.p, data.p + data.n);
    m.artwork = std::move(a);
}

void parseFlacPicture(Bytes b, uint64_t fileOffset, bool addressable, TrackMetadata& m, const ParseOptions& o) {
    size_t p = 0;
    const uint8_t type = static_cast<uint8_t>(b.be32(p));
    p += 4;
    const uint32_t mimeLen = b.be32(p);
    p += 4;
    if (!b.has(p, mimeLen)) return;
    const std::string mime = latin1(b.sub(p, mimeLen));
    p += mimeLen;
    const uint32_t descLen = b.be32(p);
    p += 4;
    if (!b.has(p, descLen)) return;
    p += descLen + 16;  // width, height, depth, colours
    const uint32_t dataLen = b.be32(p);
    p += 4;
    if (!b.has(p, dataLen)) return;
    setArtwork(m, mime, type, b.sub(p, dataLen), fileOffset + p, addressable, o);
}

// ---------------------------------------------------------------- ID3v2
Bytes stripUnsync(Bytes b, std::vector<uint8_t>& store) {
    store.clear();
    store.reserve(b.n);
    for (size_t i = 0; i < b.n; ++i) {
        store.push_back(b.p[i]);
        if (b.p[i] == 0xFF && i + 1 < b.n && b.p[i + 1] == 0x00) ++i;
    }
    return Bytes{store.data(), store.size()};
}

std::string id3Text(Bytes f) {
    if (f.n == 0) return {};
    const uint8_t enc = f.p[0];
    Bytes body = f.sub(1, f.n - 1);
    std::string s;
    switch (enc) {
        case 0: s = latin1(body); break;
        case 1: s = utf16(body, false); break;
        case 2: s = utf16(body, true); break;
        default: s = utf8(body); break;
    }
    return trim(s);
}

/// Length of an encoded NUL-terminated string at the start of b (including the terminator).
size_t id3StrLen(Bytes b, uint8_t enc) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < b.n; i += 2)
            if (b.p[i] == 0 && b.p[i + 1] == 0) return i + 2;
        return b.n;
    }
    for (size_t i = 0; i < b.n; ++i)
        if (b.p[i] == 0) return i + 1;
    return b.n;
}

/// Parses an ID3v2 tag held in `tag` (starting at "ID3"). `fileOffset` = where `tag` starts in the file.
void parseId3v2(Bytes tag, uint64_t fileOffset, TrackMetadata& m, const ParseOptions& o) {
    if (!tag.is(0, "ID3") || tag.n < 10) return;
    const uint8_t ver = tag.u8(3);
    const uint8_t flags = tag.u8(5);
    if (ver < 2 || ver > 4) return;
    const size_t size = tag.syncsafe(6);
    Bytes body = tag.sub(10, size);
    uint64_t bodyOffset = fileOffset + 10;
    std::vector<uint8_t> unsyncStore;
    bool addressable = true;
    if ((flags & 0x80) && ver < 4) {  // whole-tag unsynchronisation (v2.2/2.3)
        body = stripUnsync(body, unsyncStore);
        addressable = false;
    }
    size_t p = 0;
    if (flags & 0x40) {  // extended header
        if (ver == 3) p = 4 + body.be32(0);
        else if (ver == 4) p = body.syncsafe(0);
        if (p > body.n) return;
    }
    const size_t idLen = ver == 2 ? 3 : 4, hdrLen = ver == 2 ? 6 : 10;
    for (int guard = 0; guard < 2048 && body.has(p, hdrLen); ++guard) {
        if (body.p[p] == 0) break;  // padding
        const std::string id(reinterpret_cast<const char*>(body.p + p), idLen);
        size_t fsize;
        uint16_t fflags = 0;
        if (ver == 2) fsize = body.be24(p + 3);
        else if (ver == 3) fsize = body.be32(p + 4);
        else {
            fsize = body.syncsafe(p + 4);
            // Some taggers write plain sizes in v2.4; accept them when the syncsafe reading doesn't fit.
            const uint32_t plain = body.be32(p + 4);
            if (!body.has(p + hdrLen, fsize) && body.has(p + hdrLen, plain)) fsize = plain;
        }
        if (ver >= 3) fflags = body.be16(p + 8);
        const size_t start = p + hdrLen;
        if (!body.has(start, fsize)) break;
        Bytes f = body.sub(start, fsize);
        uint64_t fOffset = bodyOffset + start;
        bool fAddressable = addressable;
        p = start + fsize;

        if (ver == 3 && (fflags & 0x00C0)) continue;  // compressed / encrypted
        std::vector<uint8_t> frameStore;
        if (ver == 4) {
            if (fflags & 0x000C) continue;  // compressed / encrypted
            if (fflags & 0x0001) { f = f.sub(4, f.n); fOffset += 4; }  // data length indicator
            if (fflags & 0x0002) { f = stripUnsync(f, frameStore); fAddressable = false; }
        }

        const auto is = [&](const char* a, const char* b2) { return id == a || id == b2; };
        if (is("TIT2", "TT2")) setIfEmpty(m.title, id3Text(f));
        else if (is("TPE1", "TP1")) setIfEmpty(m.artist, id3Text(f));
        else if (is("TALB", "TAL")) setIfEmpty(m.album, id3Text(f));
        else if (is("TPE2", "TP2")) setIfEmpty(m.albumArtist, id3Text(f));
        else if (is("TCOM", "TCM")) setIfEmpty(m.composer, id3Text(f));
        else if (is("TCON", "TCO")) setIfEmpty(m.genre, genreName(id3Text(f)));
        else if (is("TYER", "TYE") || is("TDRC", "TDOR")) { if (!m.year) m.year = parseYear(id3Text(f)); }
        else if (is("TRCK", "TRK")) parseNumberPair(id3Text(f), m.trackNo, m.trackTotal);
        else if (is("TPOS", "TPA")) parseNumberPair(id3Text(f), m.discNo, m.discTotal);
        else if (is("TXXX", "TXX") && f.n > 1) {
            const uint8_t enc = f.p[0];
            Bytes rest = f.sub(1, f.n - 1);
            const size_t dl = id3StrLen(rest, enc);
            std::vector<uint8_t> d1(f.p, f.p + 1), d2(f.p, f.p + 1);
            d1.insert(d1.end(), rest.p, rest.p + dl);
            d2.insert(d2.end(), rest.p + std::min(dl, rest.n), rest.p + rest.n);
            const std::string k = id3Text(Bytes{d1.data(), d1.size()}), v = id3Text(Bytes{d2.data(), d2.size()});
            if (!k.empty() && m.extra.size() < 64) m.extra.emplace_back(k, v);
        } else if (is("APIC", "PIC") && f.n > 4) {
            const uint8_t enc = f.p[0];
            size_t q = 1;
            std::string mime;
            if (ver == 2) { mime = latin1(f.sub(1, 3)); q = 4; }
            else { const size_t ml = id3StrLen(f.sub(1, f.n - 1), 0); mime = latin1(f.sub(1, ml)); q = 1 + ml; }
            const uint8_t type = f.u8(q);
            ++q;
            q += id3StrLen(f.sub(q, f.n - std::min(q, f.n)), enc);
            if (q <= f.n) setArtwork(m, mime, type, f.sub(q, f.n - q), fOffset + q, fAddressable, o);
        }
    }
}

void parseId3v1(Bytes t, TrackMetadata& m) {
    if (!t.is(0, "TAG") || t.n < 128) return;
    setIfEmpty(m.title, trim(latin1(t.sub(3, 30))));
    setIfEmpty(m.artist, trim(latin1(t.sub(33, 30))));
    setIfEmpty(m.album, trim(latin1(t.sub(63, 30))));
    if (!m.year) m.year = parseYear(latin1(t.sub(93, 4)));
    if (t.u8(125) == 0 && t.u8(126) && !m.trackNo) m.trackNo = t.u8(126);
    if (m.genre.empty() && t.u8(127) < sizeof kId3Genres / sizeof *kId3Genres) m.genre = kId3Genres[t.u8(127)];
}

// ---------------------------------------------------------------- per-container parsers
Result<void> parseFlac(ByteSource& src, uint64_t start, TrackMetadata& m, const ParseOptions& o) {
    m.container = "FLAC";
    m.codec = "FLAC";
    m.lossless = true;
    uint64_t off = start + 4;
    for (int guard = 0; guard < 128; ++guard) {
        auto hr = readRange(src, off, 4);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        if (h.n < 4) break;
        const bool last = h.p[0] & 0x80;
        const uint8_t type = h.p[0] & 0x7F;
        const uint32_t len = h.be24(1);
        const uint64_t body = off + 4;
        if (type == 0 || type == 4 || (type == 6 && len <= o.maxTagBytes)) {
            auto br = readRange(src, body, len);
            if (!br) return br.error();
            Bytes b{br.value().data(), br.value().size()};
            if (type == 0 && b.n >= 18) {  // STREAMINFO
                m.sampleRate = b.be24(10) >> 4;
                m.channels = static_cast<uint16_t>(((b.u8(12) >> 1) & 7) + 1);
                m.bitDepth = static_cast<uint16_t>((((b.u8(12) & 1) << 4) | (b.u8(13) >> 4)) + 1);
                const uint64_t total = (uint64_t(b.u8(13) & 0x0F) << 32) | b.be32(14);
                if (m.sampleRate) m.durationSeconds = static_cast<double>(total) / m.sampleRate;
            } else if (type == 4) {
                parseVorbisComments(b, m, o);
            } else if (type == 6) {
                parseFlacPicture(b, body, true, m, o);
            }
        }
        off = body + len;
        if (last) break;
    }
    if (m.durationSeconds > 0) m.bitrateKbps = static_cast<uint32_t>(src.size() * 8 / m.durationSeconds / 1000);
    return Result<void>::success();
}

void parseMp3Stream(ByteSource& src, uint64_t start, TrackMetadata& m) {
    m.container = "MP3";
    m.codec = "MP3";
    m.lossless = false;
    auto r = readRange(src, start, 64 * 1024);
    if (!r) return;
    Bytes b{r.value().data(), r.value().size()};
    static const int kBitrates[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static const int kRates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    for (size_t i = 0; i + 4 <= b.n; ++i) {
        if (b.p[i] != 0xFF || (b.p[i + 1] & 0xE0) != 0xE0) continue;
        const int verBits = (b.p[i + 1] >> 3) & 3, layerBits = (b.p[i + 1] >> 1) & 3;
        const int brIdx = b.p[i + 2] >> 4, srIdx = (b.p[i + 2] >> 2) & 3;
        if (verBits == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3) continue;
        const int layer = 4 - layerBits;  // 1..3
        const bool v1 = verBits == 3;
        const int rate = kRates[verBits][srIdx];
        const int kbps = kBitrates[v1 ? 0 : 1][layer - 1][brIdx];
        const bool mono = ((b.p[i + 3] >> 6) & 3) == 3;
        m.sampleRate = static_cast<uint32_t>(rate);
        m.channels = mono ? 1 : 2;
        const int samplesPerFrame = layer == 1 ? 384 : (layer == 3 && !v1) ? 576 : 1152;
        // Xing/Info (VBR) or VBRI header inside the first frame gives the exact frame count.
        const size_t xing = i + 4 + (v1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        uint32_t frames = 0;
        if ((b.is(xing, "Xing") || b.is(xing, "Info")) && (b.be32(xing + 4) & 1)) frames = b.be32(xing + 8);
        else if (b.is(i + 36, "VBRI")) frames = b.be32(i + 36 + 14);
        const uint64_t audioBytes = src.size() > start + i ? src.size() - start - i : 0;
        if (frames && rate) {
            m.durationSeconds = static_cast<double>(frames) * samplesPerFrame / rate;
            if (m.durationSeconds > 0) m.bitrateKbps = static_cast<uint32_t>(audioBytes * 8 / m.durationSeconds / 1000);
        } else if (kbps) {
            m.bitrateKbps = static_cast<uint32_t>(kbps);
            m.durationSeconds = static_cast<double>(audioBytes) * 8 / (kbps * 1000.0);
        }
        return;
    }
}

Result<void> parseMp4(ByteSource& src, TrackMetadata& m, const ParseOptions& o) {
    m.container = "MP4";
    uint64_t off = 0, moovOff = 0, moovLen = 0;
    for (int guard = 0; guard < 512 && off + 8 <= src.size(); ++guard) {
        auto hr = readRange(src, off, 16);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        uint64_t size = h.be32(0);
        uint64_t hdr = 8;
        if (size == 1) { size = h.be64(8); hdr = 16; }
        else if (size == 0) size = src.size() - off;
        if (size < hdr) break;
        if (h.is(4, "moov")) { moovOff = off + hdr; moovLen = size - hdr; break; }
        off += size;
    }
    if (!moovLen || moovLen > o.maxTagBytes) return Result<void>::success();
    auto mr = readRange(src, moovOff, static_cast<size_t>(moovLen));
    if (!mr) return mr.error();
    const std::vector<uint8_t>& mv = mr.value();
    Bytes b{mv.data(), mv.size()};

    struct Atom { std::string type; size_t start, end; };
    auto children = [&](size_t s, size_t e) {
        std::vector<Atom> out;
        size_t p = s;
        while (p + 8 <= e && out.size() < 4096) {
            uint64_t size = b.be32(p);
            size_t hdr = 8;
            if (size == 1) { size = b.be64(p + 8); hdr = 16; }
            else if (size == 0) size = e - p;
            if (size < hdr || size > e - p) break;
            out.push_back({std::string(reinterpret_cast<const char*>(b.p + p + 4), 4), p + hdr, p + static_cast<size_t>(size)});
            p += static_cast<size_t>(size);
        }
        return out;
    };
    auto find = [&](size_t s, size_t e, std::initializer_list<const char*> path) -> std::optional<Atom> {
        Atom cur{"", s, e};
        for (const char* t : path) {
            auto kids = children(cur.start, cur.end);
            auto it = std::find_if(kids.begin(), kids.end(), [&](const Atom& a) { return a.type == t; });
            if (it == kids.end()) return std::nullopt;
            cur = *it;
            if (cur.type == "meta") cur.start = std::min(cur.start + 4, cur.end);  // full box
        }
        return cur;
    };

    for (const Atom& trak : children(0, b.n)) {
        if (trak.type != "trak") continue;
        auto hdlr = find(trak.start, trak.end, {"mdia", "hdlr"});
        if (!hdlr || !b.is(hdlr->start + 8, "soun")) continue;
        if (auto mdhd = find(trak.start, trak.end, {"mdia", "mdhd"})) {
            const bool v1 = b.u8(mdhd->start) == 1;
            const uint32_t ts = v1 ? b.be32(mdhd->start + 20) : b.be32(mdhd->start + 12);
            const uint64_t dur = v1 ? b.be64(mdhd->start + 24) : b.be32(mdhd->start + 16);
            if (ts) m.durationSeconds = static_cast<double>(dur) / ts;
        }
        if (auto stsd = find(trak.start, trak.end, {"mdia", "minf", "stbl", "stsd"})) {
            const size_t e = stsd->start + 8;
            const size_t entryEnd = std::min(stsd->end, e + b.be32(e));
            const std::string fmt = b.has(e + 4, 4) ? std::string(reinterpret_cast<const char*>(b.p + e + 4), 4) : "";
            m.channels = b.be16(e + 24);
            m.bitDepth = b.be16(e + 26);
            m.sampleRate = b.be32(e + 32) >> 16;
            if (fmt == "alac") {
                m.codec = "ALAC";
                m.lossless = true;
                for (const Atom& a : children(std::min(e + 36, entryEnd), entryEnd))
                    if (a.type == "alac") {
                        const size_t c = a.start + 4;
                        m.bitDepth = b.u8(c + 5);
                        m.channels = b.u8(c + 9);
                        if (const uint32_t br = b.be32(c + 16)) m.bitrateKbps = br / 1000;
                        m.sampleRate = b.be32(c + 20);
                    }
            } else if (fmt == "mp4a") {
                m.codec = "AAC";
                m.lossless = false;
                m.bitDepth = 0;
                for (const Atom& a : children(std::min(e + 36, entryEnd), entryEnd))
                    if (a.type == "esds")
                        for (size_t p = a.start + 4; p + 14 < a.end; ++p)
                            if (b.u8(p) == 0x04) {
                                size_t q = p + 1;
                                while (q < a.end && (b.u8(q) & 0x80)) ++q;
                                if (const uint32_t avg = b.be32(q + 1 + 9)) m.bitrateKbps = avg / 1000;
                                break;
                            }
            } else if (fmt == "fLaC") { m.codec = "FLAC"; m.lossless = true; }
            else if (fmt == "Opus") { m.codec = "Opus"; m.lossless = false; m.bitDepth = 0; }
            else if (!fmt.empty()) m.codec = fmt;
        }
        break;
    }

    if (auto ilst = find(0, b.n, {"udta", "meta", "ilst"})) {
        for (const Atom& item : children(ilst->start, ilst->end)) {
            const auto kids = children(item.start, item.end);
            auto data = std::find_if(kids.begin(), kids.end(), [](const Atom& a) { return a.type == "data"; });
            if (data == kids.end() || data->end < data->start + 8) continue;
            const uint32_t kind = b.be32(data->start) & 0xFFFFFF;
            Bytes payload = b.sub(data->start + 8, data->end - data->start - 8);
            const std::string text = trim(utf8(payload));
            const std::string& t = item.type;
            if (t == "\xA9nam") setIfEmpty(m.title, text);
            else if (t == "\xA9" "ART") setIfEmpty(m.artist, text);
            else if (t == "\xA9" "alb") setIfEmpty(m.album, text);
            else if (t == "aART") setIfEmpty(m.albumArtist, text);
            else if (t == "\xA9wrt") setIfEmpty(m.composer, text);
            else if (t == "\xA9" "day") { if (!m.year) m.year = parseYear(text); }
            else if (t == "\xA9gen") setIfEmpty(m.genre, text);
            else if (t == "gnre" && payload.n >= 2) { const int g = payload.be16(0) - 1; if (g >= 0 && g < 80 && m.genre.empty()) m.genre = kId3Genres[g]; }
            else if (t == "trkn" && payload.n >= 6) { if (!m.trackNo) m.trackNo = payload.be16(2); if (!m.trackTotal) m.trackTotal = payload.be16(4); }
            else if (t == "disk" && payload.n >= 6) { if (!m.discNo) m.discNo = payload.be16(2); if (!m.discTotal) m.discTotal = payload.be16(4); }
            else if (t == "covr") setArtwork(m, kind == 14 ? "image/png" : "image/jpeg", 3, payload, moovOff + (payload.p - b.p), true, o);
            else if (t == "----") {
                auto nm = std::find_if(kids.begin(), kids.end(), [](const Atom& a) { return a.type == "name"; });
                if (nm != kids.end() && nm->end > nm->start + 4 && m.extra.size() < 64)
                    m.extra.emplace_back(utf8(b.sub(nm->start + 4, nm->end - nm->start - 4)), text);
            }
        }
    }
    if (!m.bitrateKbps && m.durationSeconds > 0) m.bitrateKbps = static_cast<uint32_t>(src.size() * 8 / m.durationSeconds / 1000);
    return Result<void>::success();
}

Result<void> parseRiff(ByteSource& src, TrackMetadata& m, const ParseOptions& o) {
    m.container = "WAV";
    uint64_t off = 12, byteRate = 0, dataSize = 0;
    for (int guard = 0; guard < 256 && off + 8 <= src.size(); ++guard) {
        auto hr = readRange(src, off, 8);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        const uint32_t size = h.le32(4);
        const uint64_t body = off + 8;
        if (h.is(0, "fmt ")) {
            auto fr = readRange(src, body, std::min<uint32_t>(size, 64));
            if (!fr) return fr.error();
            Bytes f{fr.value().data(), fr.value().size()};
            uint16_t tag = f.le16(0);
            if (tag == 0xFFFE && f.n >= 26) tag = f.le16(24);  // WAVE_FORMAT_EXTENSIBLE sub-format
            m.channels = f.le16(2);
            m.sampleRate = f.le32(4);
            byteRate = f.le32(8);
            m.bitDepth = f.le16(14);
            m.codec = tag == 1 ? "PCM" : tag == 3 ? "PCM float" : "WAV";
            m.lossless = tag == 1 || tag == 3;
        } else if (h.is(0, "data")) {
            dataSize = std::min<uint64_t>(size, src.size() - std::min(src.size(), body));
        } else if (h.is(0, "LIST") && size < (1u << 20)) {
            auto lr = readRange(src, body, size);
            if (!lr) return lr.error();
            Bytes l{lr.value().data(), lr.value().size()};
            if (l.is(0, "INFO"))
                for (size_t p = 4; p + 8 <= l.n;) {
                    const uint32_t ls = l.le32(p + 4);
                    const std::string v = trim(utf8(l.sub(p + 8, ls)));
                    if (l.is(p, "INAM")) setIfEmpty(m.title, v);
                    else if (l.is(p, "IART")) setIfEmpty(m.artist, v);
                    else if (l.is(p, "IPRD")) setIfEmpty(m.album, v);
                    else if (l.is(p, "IGNR")) setIfEmpty(m.genre, v);
                    else if (l.is(p, "ICRD")) { if (!m.year) m.year = parseYear(v); }
                    else if (l.is(p, "ITRK") || l.is(p, "IPRT")) { int t = 0; parseNumberPair(v, m.trackNo, t); }
                    if (ls > l.n) break;
                    p += 8 + ls + (ls & 1);
                }
        } else if ((h.is(0, "id3 ") || h.is(0, "ID3 ")) && size <= o.maxTagBytes) {
            auto ir = readRange(src, body, size);
            if (!ir) return ir.error();
            parseId3v2(Bytes{ir.value().data(), ir.value().size()}, body, m, o);
        }
        off = body + size + (size & 1);
    }
    if (byteRate && dataSize) m.durationSeconds = static_cast<double>(dataSize) / byteRate;
    if (byteRate) m.bitrateKbps = static_cast<uint32_t>(byteRate * 8 / 1000);
    return Result<void>::success();
}

double ieeeExtended(Bytes b, size_t o) {  // 80-bit big-endian float (AIFF sample rate)
    if (!b.has(o, 10)) return 0;
    const int exp = ((b.p[o] & 0x7F) << 8 | b.p[o + 1]) - 16383;
    const uint64_t mant = b.be64(o + 2);
    if (exp < -64 || exp > 64 || mant == 0) return 0;
    return std::ldexp(static_cast<double>(mant), exp - 63);
}

Result<void> parseAiff(ByteSource& src, TrackMetadata& m, const ParseOptions& o, bool aifc) {
    m.container = "AIFF";
    m.codec = "PCM";
    m.lossless = true;
    uint64_t off = 12, frames = 0;
    for (int guard = 0; guard < 256 && off + 8 <= src.size(); ++guard) {
        auto hr = readRange(src, off, 8);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        const uint32_t size = h.be32(4);
        const uint64_t body = off + 8;
        if (h.is(0, "COMM")) {
            auto cr = readRange(src, body, std::min<uint32_t>(size, 64));
            if (!cr) return cr.error();
            Bytes c{cr.value().data(), cr.value().size()};
            m.channels = c.be16(0);
            frames = c.be32(2);
            m.bitDepth = c.be16(6);
            m.sampleRate = static_cast<uint32_t>(std::lround(ieeeExtended(c, 8)));
            if (aifc && c.has(18, 4)) {
                const std::string comp(reinterpret_cast<const char*>(c.p + 18), 4);
                if (comp == "fl32" || comp == "FL32") m.codec = "PCM float";
                else if (comp != "NONE" && comp != "sowt" && comp != "twos") { m.codec = comp; m.lossless = false; }
            }
        } else if (h.is(0, "NAME") && size < 4096) {
            auto r = readRange(src, body, size);
            if (r) setIfEmpty(m.title, trim(latin1(Bytes{r.value().data(), r.value().size()})));
        } else if (h.is(0, "AUTH") && size < 4096) {
            auto r = readRange(src, body, size);
            if (r) setIfEmpty(m.artist, trim(latin1(Bytes{r.value().data(), r.value().size()})));
        } else if ((h.is(0, "ID3 ") || h.is(0, "id3 ")) && size <= o.maxTagBytes) {
            auto r = readRange(src, body, size);
            if (!r) return r.error();
            parseId3v2(Bytes{r.value().data(), r.value().size()}, body, m, o);
        }
        off = body + size + (size & 1);
    }
    if (m.sampleRate && frames) m.durationSeconds = static_cast<double>(frames) / m.sampleRate;
    if (m.sampleRate && m.bitDepth && m.channels) m.bitrateKbps = m.sampleRate * m.bitDepth * m.channels / 1000;
    return Result<void>::success();
}

Result<void> parseOgg(ByteSource& src, TrackMetadata& m, const ParseOptions& o) {
    m.container = "Ogg";
    // Reassemble the first packets from the first pages (identification + comment header).
    std::vector<std::vector<uint8_t>> packets(1);
    uint64_t off = 0;
    for (int page = 0; page < 64 && packets.size() <= 2 && off + 27 <= src.size(); ++page) {
        auto hr = readRange(src, off, 27 + 255);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        if (!h.is(0, "OggS")) break;
        const uint8_t segs = h.u8(26);
        if (!h.has(27, segs)) break;
        uint64_t data = off + 27 + segs;
        for (uint8_t s = 0; s < segs; ++s) {
            const uint8_t len = h.p[27 + s];
            if (packets.back().size() + len > o.maxTagBytes) return Result<void>::success();
            auto dr = readRange(src, data, len);
            if (!dr) return dr.error();
            packets.back().insert(packets.back().end(), dr.value().begin(), dr.value().end());
            data += len;
            if (len < 255) packets.emplace_back();
        }
        off = data;
    }
    if (packets.empty()) return Result<void>::success();
    Bytes id{packets[0].data(), packets[0].size()};
    uint32_t granRate = 0;
    uint16_t preskip = 0;
    if (id.is(0, "\x01vorbis")) {
        m.codec = "Vorbis";
        m.channels = id.u8(11);
        m.sampleRate = granRate = id.le32(12);
        if (const uint32_t nominal = id.le32(20)) m.bitrateKbps = nominal / 1000;
    } else if (id.is(0, "OpusHead")) {
        m.codec = "Opus";
        m.channels = id.u8(9);
        preskip = id.le16(10);
        m.sampleRate = id.le32(12);  // original input rate; Opus always decodes at 48 kHz
        granRate = 48000;
    } else if (id.is(0, "\x7F" "FLAC")) {
        m.codec = "FLAC";
        m.lossless = true;
        // 0x7F "FLAC" (5) + version (2) + header count (2), then "fLaC" + STREAMINFO block header (4) + body.
        Bytes si = id.sub(9, id.n);
        if (si.is(0, "fLaC")) {
            Bytes b = si.sub(8, si.n);
            m.sampleRate = granRate = b.be24(10) >> 4;
            m.channels = static_cast<uint16_t>(((b.u8(12) >> 1) & 7) + 1);
            m.bitDepth = static_cast<uint16_t>((((b.u8(12) & 1) << 4) | (b.u8(13) >> 4)) + 1);
        }
    }
    if (packets.size() > 1) {
        Bytes c{packets[1].data(), packets[1].size()};
        if (c.is(0, "\x03vorbis")) parseVorbisComments(c.sub(7, c.n), m, o);
        else if (c.is(0, "OpusTags")) parseVorbisComments(c.sub(8, c.n), m, o);
        else if (m.codec == "FLAC" && c.n > 4 && (c.p[0] & 0x7F) == 4) parseVorbisComments(c.sub(4, c.n), m, o);
    }
    // Duration from the granule position of the last page.
    if (granRate) {
        const uint64_t tailOff = src.size() > 65536 ? src.size() - 65536 : 0;
        auto tr = readRange(src, tailOff, 65536);
        if (tr) {
            Bytes t{tr.value().data(), tr.value().size()};
            for (size_t i = t.n >= 14 ? t.n - 14 : 0; i-- > 0;)
                if (t.is(i, "OggS")) {
                    const uint64_t g = t.le64(i + 6);
                    if (g != UINT64_MAX && g > preskip) m.durationSeconds = static_cast<double>(g - preskip) / granRate;
                    break;
                }
        }
    }
    if (!m.bitrateKbps && m.durationSeconds > 0) m.bitrateKbps = static_cast<uint32_t>(src.size() * 8 / m.durationSeconds / 1000);
    return Result<void>::success();
}

Result<void> parseDsf(ByteSource& src, TrackMetadata& m, const ParseOptions& o) {
    m.container = "DSF";
    m.codec = "DSD";
    m.lossless = true;
    m.bitDepth = 1;
    auto r = readRange(src, 0, 28 + 52);
    if (!r) return r.error();
    Bytes h{r.value().data(), r.value().size()};
    if (!h.is(28, "fmt ")) return Result<void>::success();
    m.channels = static_cast<uint16_t>(h.le32(28 + 24));
    m.sampleRate = h.le32(28 + 28);
    const uint64_t samples = h.le64(28 + 36);
    if (m.sampleRate) m.durationSeconds = static_cast<double>(samples) / m.sampleRate;
    m.bitrateKbps = static_cast<uint32_t>(uint64_t(m.sampleRate) * m.channels / 1000);
    const uint64_t metaPtr = h.le64(20);
    if (metaPtr && metaPtr + 10 < src.size()) {
        auto ir = readRange(src, metaPtr, 10);
        if (ir && ir.value().size() == 10) {
            Bytes ih{ir.value().data(), 10};
            const size_t tagLen = 10 + ih.syncsafe(6);
            if (ih.is(0, "ID3") && tagLen <= o.maxTagBytes) {
                auto tr = readRange(src, metaPtr, tagLen);
                if (tr) parseId3v2(Bytes{tr.value().data(), tr.value().size()}, metaPtr, m, o);
            }
        }
    }
    return Result<void>::success();
}

Result<void> parseDff(ByteSource& src, TrackMetadata& m, const ParseOptions& o) {
    m.container = "DFF";
    m.codec = "DSD";
    m.lossless = true;
    m.bitDepth = 1;
    uint64_t off = 16, dataBytes = 0;
    for (int guard = 0; guard < 256 && off + 12 <= src.size(); ++guard) {
        auto hr = readRange(src, off, 12);
        if (!hr) return hr.error();
        Bytes h{hr.value().data(), hr.value().size()};
        const uint64_t size = h.be64(4);
        const uint64_t body = off + 12;
        if (h.is(0, "PROP") && size < (1u << 20)) {
            auto pr = readRange(src, body, static_cast<size_t>(size));
            if (!pr) return pr.error();
            Bytes p{pr.value().data(), pr.value().size()};
            for (size_t q = 4; q + 12 <= p.n;) {
                const uint64_t s = p.be64(q + 4);
                if (p.is(q, "FS  ")) m.sampleRate = p.be32(q + 12);
                else if (p.is(q, "CHNL")) m.channels = p.be16(q + 12);
                else if (p.is(q, "CMPR") && p.is(q + 12, "DST ")) m.codec = "DST";
                if (s > p.n) break;
                q += 12 + static_cast<size_t>(s) + (s & 1);
            }
        } else if (h.is(0, "DSD ")) {
            dataBytes = size;
        } else if (h.is(0, "ID3 ") && size <= o.maxTagBytes) {
            auto ir = readRange(src, body, static_cast<size_t>(size));
            if (ir) parseId3v2(Bytes{ir.value().data(), ir.value().size()}, body, m, o);
        }
        if (size > src.size()) break;
        off = body + size + (size & 1);
    }
    if (m.sampleRate && m.channels && dataBytes) m.durationSeconds = static_cast<double>(dataBytes) * 8 / m.channels / m.sampleRate;
    m.bitrateKbps = static_cast<uint32_t>(uint64_t(m.sampleRate) * m.channels / 1000);
    return Result<void>::success();
}

Result<TrackMetadata> readMetadataImpl(ByteSource& src, const ParseOptions& o) {
    TrackMetadata m;
    auto hr = readRange(src, 0, 16);
    if (!hr) return hr.error();
    Bytes h{hr.value().data(), hr.value().size()};
    if (h.n < 12) return Result<TrackMetadata>::fail(ErrorCode::Corrupt, src.name() + ": file too short");

    Result<void> r = Result<void>::success();
    if (h.is(0, "fLaC")) r = parseFlac(src, 0, m, o);
    else if (h.is(0, "RIFF") && h.is(8, "WAVE")) r = parseRiff(src, m, o);
    else if (h.is(0, "FORM") && (h.is(8, "AIFF") || h.is(8, "AIFC"))) r = parseAiff(src, m, o, h.is(8, "AIFC"));
    else if (h.is(0, "OggS")) r = parseOgg(src, m, o);
    else if (h.is(4, "ftyp")) r = parseMp4(src, m, o);
    else if (h.is(0, "DSD ")) r = parseDsf(src, m, o);
    else if (h.is(0, "FRM8")) r = parseDff(src, m, o);
    else if (h.is(0, "ID3")) {
        const uint64_t tagLen = 10 + h.syncsafe(6) + ((h.u8(5) & 0x10) ? 10 : 0);
        if (tagLen <= o.maxTagBytes) {
            auto tr = readRange(src, 0, static_cast<size_t>(tagLen));
            if (!tr) return tr.error();
            parseId3v2(Bytes{tr.value().data(), tr.value().size()}, 0, m, o);
        }
        auto ar = readRange(src, tagLen, 4);
        if (ar && ar.value().size() == 4 && std::memcmp(ar.value().data(), "fLaC", 4) == 0) r = parseFlac(src, tagLen, m, o);
        else parseMp3Stream(src, tagLen, m);
    } else if (h.u8(0) == 0xFF && (h.u8(1) & 0xE0) == 0xE0) {
        parseMp3Stream(src, 0, m);
    } else {
        return Result<TrackMetadata>::fail(ErrorCode::Unsupported, src.name() + ": unknown format");
    }
    if (!r) return r.error();
    if (m.container == "MP3" && src.size() >= 128) {
        auto t = readRange(src, src.size() - 128, 128);
        if (t) parseId3v1(Bytes{t.value().data(), t.value().size()}, m);
    }
    return m;
}

}  // namespace

Result<TrackMetadata> readMetadata(ByteSource& src, const ParseOptions& opts) noexcept {
    try {
        return readMetadataImpl(src, opts);
    } catch (const std::bad_alloc&) {
        return Result<TrackMetadata>::fail(ErrorCode::OutOfMemory, src.name() + ": out of memory reading tags");
    } catch (...) {
        return Result<TrackMetadata>::fail(ErrorCode::Corrupt, src.name() + ": malformed tags");
    }
}

Result<std::vector<uint8_t>> readArtwork(ByteSource& src, const ArtworkRef& ref) noexcept {
    try {
        if (!ref.data.empty()) return ref.data;
        if (ref.inlineOnly || ref.size == 0) return Result<std::vector<uint8_t>>::fail(ErrorCode::InvalidArgument, "artwork not addressable");
        auto r = readRange(src, ref.offset, static_cast<size_t>(ref.size));
        if (!r) return r.error();
        if (r.value().size() != ref.size) return Result<std::vector<uint8_t>>::fail(ErrorCode::Io, "artwork truncated");
        return r;
    } catch (...) {
        return Result<std::vector<uint8_t>>::fail(ErrorCode::OutOfMemory, "artwork");
    }
}

}  // namespace groove
