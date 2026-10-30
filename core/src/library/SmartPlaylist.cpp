// Smart playlist → SQL. Column names and operators come only from the whitelists below; every value is a
// bound parameter, so a rule's text can never change the query (no SQL injection, tested).
#include <new>

#include "groove/Library.h"

namespace groove {
namespace {

enum class Kind { Text, Number, Bool };

struct FieldInfo {
    const char* column;
    Kind kind;
};

FieldInfo info(Field f) noexcept {
    switch (f) {
        case Field::Title: return {"title", Kind::Text};
        case Field::Artist: return {"artist", Kind::Text};
        case Field::Album: return {"album", Kind::Text};
        case Field::AlbumArtist: return {"album_artist", Kind::Text};
        case Field::Genre: return {"genre", Kind::Text};
        case Field::Composer: return {"composer", Kind::Text};
        case Field::Container: return {"container", Kind::Text};
        case Field::Codec: return {"codec", Kind::Text};
        case Field::Folder: return {"folder", Kind::Text};
        case Field::Path: return {"path", Kind::Text};
        case Field::Year: return {"year", Kind::Number};
        case Field::TrackNo: return {"track_no", Kind::Number};
        case Field::DiscNo: return {"disc_no", Kind::Number};
        case Field::SampleRate: return {"sample_rate", Kind::Number};
        case Field::BitDepth: return {"bit_depth", Kind::Number};
        case Field::Channels: return {"channels", Kind::Number};
        case Field::Duration: return {"duration", Kind::Number};
        case Field::Bitrate: return {"bitrate", Kind::Number};
        case Field::AddedAt: return {"added_at", Kind::Number};
        case Field::Lossless: return {"lossless", Kind::Bool};
    }
    return {nullptr, Kind::Text};
}

bool isNumber(const Value& v) { return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v); }

std::string likeEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

}  // namespace

Result<BuiltQuery> buildSmartQuery(const SmartPlaylist& p) noexcept {
    using R = Result<BuiltQuery>;
    try {
        if (p.rules.size() > 64) return R::fail(ErrorCode::InvalidArgument, "too many rules");
        BuiltQuery q;
        std::string where;
        for (const Rule& r : p.rules) {
            const FieldInfo fi = info(r.field);
            if (!fi.column) return R::fail(ErrorCode::InvalidArgument, "unknown field");
            const std::string col = fi.column;
            std::string clause;
            switch (fi.kind) {
                case Kind::Text: {
                    const auto* s = std::get_if<std::string>(&r.a);
                    if (!s) return R::fail(ErrorCode::InvalidArgument, col + ": text value required");
                    if (s->size() > 1024) return R::fail(ErrorCode::InvalidArgument, col + ": value too long");
                    switch (r.op) {
                        case Op::Equals: clause = col + " = ? COLLATE NOCASE"; q.params.emplace_back(*s); break;
                        case Op::NotEquals: clause = "COALESCE(" + col + ",'') <> ? COLLATE NOCASE"; q.params.emplace_back(*s); break;
                        case Op::Contains: clause = col + " LIKE ? ESCAPE '\\'"; q.params.emplace_back("%" + likeEscape(*s) + "%"); break;
                        case Op::NotContains: clause = "COALESCE(" + col + ",'') NOT LIKE ? ESCAPE '\\'"; q.params.emplace_back("%" + likeEscape(*s) + "%"); break;
                        case Op::StartsWith: clause = col + " LIKE ? ESCAPE '\\'"; q.params.emplace_back(likeEscape(*s) + "%"); break;
                        default: return R::fail(ErrorCode::InvalidArgument, col + ": operator not valid for text");
                    }
                    break;
                }
                case Kind::Number: {
                    if (!isNumber(r.a)) return R::fail(ErrorCode::InvalidArgument, col + ": number required");
                    const char* op = nullptr;
                    switch (r.op) {
                        case Op::Equals: op = " = ?"; break;
                        case Op::NotEquals: op = " <> ?"; break;
                        case Op::Greater: op = " > ?"; break;
                        case Op::GreaterOrEqual: op = " >= ?"; break;
                        case Op::Less: op = " < ?"; break;
                        case Op::LessOrEqual: op = " <= ?"; break;
                        case Op::Between:
                            if (!isNumber(r.b)) return R::fail(ErrorCode::InvalidArgument, col + ": BETWEEN needs two numbers");
                            clause = col + " BETWEEN ? AND ?";
                            q.params.push_back(r.a);
                            q.params.push_back(r.b);
                            break;
                        default: return R::fail(ErrorCode::InvalidArgument, col + ": operator not valid for numbers");
                    }
                    if (op) {
                        clause = col + op;
                        q.params.push_back(r.a);
                    }
                    break;
                }
                case Kind::Bool:
                    if (r.op == Op::IsTrue) clause = col + " = 1";
                    else if (r.op == Op::IsFalse) clause = "COALESCE(" + col + ",0) = 0";
                    else return R::fail(ErrorCode::InvalidArgument, col + ": use IsTrue / IsFalse");
                    break;
            }
            if (!where.empty()) where += p.matchAll ? " AND " : " OR ";
            where += "(" + clause + ")";
        }
        if (!where.empty()) q.sql = " WHERE " + where;
        if (p.random) {
            q.sql += " ORDER BY random()";
        } else {
            const FieldInfo s = info(p.sortBy);
            if (!s.column) return R::fail(ErrorCode::InvalidArgument, "unknown sort field");
            const std::string dir = p.descending ? " DESC" : " ASC";
            q.sql += std::string(" ORDER BY ") + s.column + (s.kind == Kind::Text ? " COLLATE NOCASE" : "") + dir +
                     ", album_artist COLLATE NOCASE, album COLLATE NOCASE, disc_no, track_no, title COLLATE NOCASE, id";
        }
        if (p.limit > 0) {
            q.sql += " LIMIT ?";
            q.params.emplace_back(static_cast<int64_t>(p.limit));
        }
        return R(std::move(q));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "smart playlist");
    }
}

}  // namespace groove
