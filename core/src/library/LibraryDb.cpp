#include <sqlite3.h>

#include <chrono>
#include <new>

#include "groove/Library.h"

namespace groove {
namespace {

constexpr int kSchemaVersion = 1;

struct StmtFinalizer {
    void operator()(sqlite3_stmt* s) const noexcept { sqlite3_finalize(s); }
};
using Stmt = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

Error dbError(sqlite3* db, const char* what) {
    return Error{ErrorCode::Io, std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "sqlite")};
}

Result<Stmt> prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &raw, nullptr) != SQLITE_OK) {
        sqlite3_finalize(raw);
        return dbError(db, "prepare");
    }
    return Stmt(raw);
}

void bindText(sqlite3_stmt* s, int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }

void bindValue(sqlite3_stmt* s, int i, const Value& v) {
    if (auto p = std::get_if<int64_t>(&v)) sqlite3_bind_int64(s, i, *p);
    else if (auto d = std::get_if<double>(&v)) sqlite3_bind_double(s, i, *d);
    else if (auto t = std::get_if<std::string>(&v)) bindText(s, i, *t);
    else sqlite3_bind_null(s, i);
}

std::string colText(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, i))) : std::string();
}

const char* kColumns =
    "id, path, root, folder, title, artist, album, album_artist, genre, composer, container, codec, year, track_no, disc_no, "
    "sample_rate, bitrate, bit_depth, channels, duration, lossless, has_art, file_size, mtime, added_at";

LibraryTrack rowToTrack(sqlite3_stmt* s) {
    LibraryTrack t;
    t.id = sqlite3_column_int64(s, 0);
    t.path = colText(s, 1);
    t.root = colText(s, 2);
    t.folder = colText(s, 3);
    t.title = colText(s, 4);
    t.artist = colText(s, 5);
    t.album = colText(s, 6);
    t.albumArtist = colText(s, 7);
    t.genre = colText(s, 8);
    t.composer = colText(s, 9);
    t.container = colText(s, 10);
    t.codec = colText(s, 11);
    t.year = sqlite3_column_int(s, 12);
    t.trackNo = sqlite3_column_int(s, 13);
    t.discNo = sqlite3_column_int(s, 14);
    t.sampleRate = static_cast<uint32_t>(sqlite3_column_int64(s, 15));
    t.bitrateKbps = static_cast<uint32_t>(sqlite3_column_int64(s, 16));
    t.bitDepth = static_cast<uint16_t>(sqlite3_column_int(s, 17));
    t.channels = static_cast<uint16_t>(sqlite3_column_int(s, 18));
    t.durationSeconds = sqlite3_column_double(s, 19);
    t.lossless = sqlite3_column_int(s, 20) != 0;
    t.hasArtwork = sqlite3_column_int(s, 21) != 0;
    t.fileSize = sqlite3_column_int64(s, 22);
    t.mtime = sqlite3_column_int64(s, 23);
    t.addedAt = sqlite3_column_int64(s, 24);
    return t;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

int LibraryDb::schemaVersion() noexcept { return kSchemaVersion; }

LibraryDb::LibraryDb(PrivateTag, sqlite3* db) noexcept : db_(db) {}

LibraryDb::~LibraryDb() {
    if (db_) sqlite3_close_v2(db_);  // finalizes lazily if a statement is still alive (none should be)
}

Result<void> LibraryDb::exec(const char* sql) noexcept {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        Error e{ErrorCode::Io, std::string("sql: ") + (err ? err : "error")};
        sqlite3_free(err);
        return e;
    }
    return Result<void>::success();
}

Result<std::unique_ptr<LibraryDb>> LibraryDb::open(const std::string& path) noexcept {
    using R = Result<std::unique_ptr<LibraryDb>>;
    try {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            Error e = dbError(raw, "open");
            sqlite3_close_v2(raw);
            return e;
        }
        // Own the handle before anything can throw, so it's closed on every path.
        std::unique_ptr<sqlite3, int (*)(sqlite3*)> guard(raw, &sqlite3_close_v2);
        auto db = std::make_unique<LibraryDb>(PrivateTag{}, raw);
        guard.release();
        sqlite3_busy_timeout(raw, 5000);  // UI connection and scanner connection wait for each other briefly
        auto r = db->exec(
            "PRAGMA journal_mode=WAL;"
            "PRAGMA synchronous=NORMAL;"
            "PRAGMA foreign_keys=ON;"
            "CREATE TABLE IF NOT EXISTS tracks ("
            " id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, root TEXT NOT NULL, folder TEXT NOT NULL DEFAULT '',"
            " title TEXT, artist TEXT, album TEXT, album_artist TEXT, genre TEXT, composer TEXT,"
            " container TEXT, codec TEXT, year INTEGER, track_no INTEGER, disc_no INTEGER,"
            " sample_rate INTEGER, bitrate INTEGER, bit_depth INTEGER, channels INTEGER, duration REAL,"
            " lossless INTEGER, has_art INTEGER, art_offset INTEGER, art_size INTEGER, art_mime TEXT,"
            " file_size INTEGER, mtime INTEGER, added_at INTEGER, scan_gen INTEGER);"
            "CREATE INDEX IF NOT EXISTS ix_tracks_root ON tracks(root, scan_gen);"
            "CREATE INDEX IF NOT EXISTS ix_tracks_artist ON tracks(artist COLLATE NOCASE);"
            "CREATE INDEX IF NOT EXISTS ix_tracks_album ON tracks(album_artist COLLATE NOCASE, album COLLATE NOCASE, disc_no, track_no);"
            "CREATE INDEX IF NOT EXISTS ix_tracks_quality ON tracks(sample_rate, bit_depth);"
            "CREATE TABLE IF NOT EXISTS extra_tags (track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE, key TEXT NOT NULL, value TEXT);"
            "CREATE INDEX IF NOT EXISTS ix_extra ON extra_tags(track_id);");
        if (!r) return r.error();
        auto v = db->exec("PRAGMA user_version=1;");
        if (!v) return v.error();
        return R(std::move(db));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "library db");
    }
}

Result<void> LibraryDb::begin() noexcept { return exec("BEGIN IMMEDIATE;"); }
Result<void> LibraryDb::commit() noexcept { return exec("COMMIT;"); }
void LibraryDb::rollback() noexcept { (void)exec("ROLLBACK;"); }

Result<std::optional<FileStamp>> LibraryDb::stamp(const std::string& path) noexcept {
    using R = Result<std::optional<FileStamp>>;
    try {
        auto s = prepare(db_, "SELECT file_size, mtime FROM tracks WHERE path = ?1");
        if (!s) return s.error();
        bindText(s.value().get(), 1, path);
        const int rc = sqlite3_step(s.value().get());
        if (rc == SQLITE_ROW) return R(std::optional<FileStamp>(FileStamp{sqlite3_column_int64(s.value().get(), 0), sqlite3_column_int64(s.value().get(), 1)}));
        if (rc == SQLITE_DONE) return R(std::optional<FileStamp>());
        return dbError(db_, "stamp");
    } catch (...) {
        return R::fail(ErrorCode::OutOfMemory, "stamp");
    }
}

Result<void> LibraryDb::touch(const std::string& path, int64_t gen) noexcept {
    try {
        auto s = prepare(db_, "UPDATE tracks SET scan_gen = ?1 WHERE path = ?2");
        if (!s) return s.error();
        sqlite3_bind_int64(s.value().get(), 1, gen);
        bindText(s.value().get(), 2, path);
        if (sqlite3_step(s.value().get()) != SQLITE_DONE) return dbError(db_, "touch");
        return Result<void>::success();
    } catch (...) {
        return Result<void>::fail(ErrorCode::OutOfMemory, "touch");
    }
}

Result<int64_t> LibraryDb::upsert(const std::string& path, const std::string& root, const std::string& folder, const FileStamp& st,
                                  const TrackMetadata& m, int64_t gen) noexcept {
    try {
        // ON CONFLICT … DO UPDATE needs SQLite ≥ 3.24; the NDK/iOS builds bundle a current amalgamation.
        auto s = prepare(db_,
                         "INSERT INTO tracks (path, root, folder, title, artist, album, album_artist, genre, composer, container, codec,"
                         " year, track_no, disc_no, sample_rate, bitrate, bit_depth, channels, duration, lossless, has_art,"
                         " art_offset, art_size, art_mime, file_size, mtime, added_at, scan_gen)"
                         " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28)"
                         " ON CONFLICT(path) DO UPDATE SET root=excluded.root, folder=excluded.folder, title=excluded.title,"
                         " artist=excluded.artist, album=excluded.album, album_artist=excluded.album_artist, genre=excluded.genre,"
                         " composer=excluded.composer, container=excluded.container, codec=excluded.codec, year=excluded.year,"
                         " track_no=excluded.track_no, disc_no=excluded.disc_no, sample_rate=excluded.sample_rate, bitrate=excluded.bitrate,"
                         " bit_depth=excluded.bit_depth, channels=excluded.channels, duration=excluded.duration, lossless=excluded.lossless,"
                         " has_art=excluded.has_art, art_offset=excluded.art_offset, art_size=excluded.art_size, art_mime=excluded.art_mime,"
                         " file_size=excluded.file_size, mtime=excluded.mtime, scan_gen=excluded.scan_gen"
                         " RETURNING id");
        if (!s) return s.error();
        sqlite3_stmt* q = s.value().get();
        bindText(q, 1, path);
        bindText(q, 2, root);
        bindText(q, 3, folder);
        bindText(q, 4, m.title);
        bindText(q, 5, m.artist);
        bindText(q, 6, m.album);
        bindText(q, 7, m.albumArtist);
        bindText(q, 8, m.genre);
        bindText(q, 9, m.composer);
        bindText(q, 10, m.container);
        bindText(q, 11, m.codec);
        sqlite3_bind_int(q, 12, m.year);
        sqlite3_bind_int(q, 13, m.trackNo);
        sqlite3_bind_int(q, 14, m.discNo);
        sqlite3_bind_int64(q, 15, m.sampleRate);
        sqlite3_bind_int64(q, 16, m.bitrateKbps);
        sqlite3_bind_int(q, 17, m.bitDepth);
        sqlite3_bind_int(q, 18, m.channels);
        sqlite3_bind_double(q, 19, m.durationSeconds);
        sqlite3_bind_int(q, 20, m.lossless ? 1 : 0);
        sqlite3_bind_int(q, 21, m.artwork ? 1 : 0);
        const bool addressable = m.artwork && !m.artwork->inlineOnly;
        sqlite3_bind_int64(q, 22, addressable ? static_cast<int64_t>(m.artwork->offset) : 0);
        sqlite3_bind_int64(q, 23, addressable ? static_cast<int64_t>(m.artwork->size) : 0);
        bindText(q, 24, m.artwork ? m.artwork->mime : std::string());
        sqlite3_bind_int64(q, 25, st.size);
        sqlite3_bind_int64(q, 26, st.mtime);
        sqlite3_bind_int64(q, 27, nowMs());
        sqlite3_bind_int64(q, 28, gen);
        if (sqlite3_step(q) != SQLITE_ROW) return dbError(db_, "upsert");
        const int64_t id = sqlite3_column_int64(q, 0);
        s.value().reset();

        auto del = prepare(db_, "DELETE FROM extra_tags WHERE track_id = ?1");
        if (!del) return del.error();
        sqlite3_bind_int64(del.value().get(), 1, id);
        if (sqlite3_step(del.value().get()) != SQLITE_DONE) return dbError(db_, "extra delete");
        if (!m.extra.empty()) {
            auto ins = prepare(db_, "INSERT INTO extra_tags (track_id, key, value) VALUES (?1, ?2, ?3)");
            if (!ins) return ins.error();
            for (const auto& kv : m.extra) {
                sqlite3_reset(ins.value().get());
                sqlite3_bind_int64(ins.value().get(), 1, id);
                bindText(ins.value().get(), 2, kv.first);
                bindText(ins.value().get(), 3, kv.second);
                if (sqlite3_step(ins.value().get()) != SQLITE_DONE) return dbError(db_, "extra insert");
            }
        }
        return id;
    } catch (...) {
        return Result<int64_t>::fail(ErrorCode::OutOfMemory, "upsert");
    }
}

Result<int64_t> LibraryDb::sweep(const std::string& root, int64_t gen) noexcept {
    try {
        auto s = prepare(db_, "DELETE FROM tracks WHERE root = ?1 AND scan_gen <> ?2");
        if (!s) return s.error();
        bindText(s.value().get(), 1, root);
        sqlite3_bind_int64(s.value().get(), 2, gen);
        if (sqlite3_step(s.value().get()) != SQLITE_DONE) return dbError(db_, "sweep");
        return static_cast<int64_t>(sqlite3_changes(db_));
    } catch (...) {
        return Result<int64_t>::fail(ErrorCode::OutOfMemory, "sweep");
    }
}

Result<void> LibraryDb::remove(int64_t id) noexcept {
    try {
        auto s = prepare(db_, "DELETE FROM tracks WHERE id = ?1");
        if (!s) return s.error();
        sqlite3_bind_int64(s.value().get(), 1, id);
        if (sqlite3_step(s.value().get()) != SQLITE_DONE) return dbError(db_, "remove");
        return Result<void>::success();
    } catch (...) {
        return Result<void>::fail(ErrorCode::OutOfMemory, "remove");
    }
}

Result<std::vector<LibraryTrack>> LibraryDb::query(const SmartPlaylist& p) noexcept {
    using R = Result<std::vector<LibraryTrack>>;
    try {
        auto bq = buildSmartQuery(p);
        if (!bq) return bq.error();
        auto s = prepare(db_, std::string("SELECT ") + kColumns + " FROM tracks" + bq.value().sql);
        if (!s) return s.error();
        for (size_t i = 0; i < bq.value().params.size(); ++i) bindValue(s.value().get(), static_cast<int>(i + 1), bq.value().params[i]);
        std::vector<LibraryTrack> out;
        int rc;
        while ((rc = sqlite3_step(s.value().get())) == SQLITE_ROW) out.push_back(rowToTrack(s.value().get()));
        if (rc != SQLITE_DONE) return dbError(db_, "query");
        return R(std::move(out));
    } catch (...) {
        return R::fail(ErrorCode::OutOfMemory, "query");
    }
}

Result<std::optional<LibraryTrack>> LibraryDb::get(int64_t id) noexcept {
    using R = Result<std::optional<LibraryTrack>>;
    try {
        auto s = prepare(db_, std::string("SELECT ") + kColumns + " FROM tracks WHERE id = ?1");
        if (!s) return s.error();
        sqlite3_bind_int64(s.value().get(), 1, id);
        const int rc = sqlite3_step(s.value().get());
        if (rc == SQLITE_ROW) return R(std::optional<LibraryTrack>(rowToTrack(s.value().get())));
        if (rc == SQLITE_DONE) return R(std::optional<LibraryTrack>());
        return dbError(db_, "get");
    } catch (...) {
        return R::fail(ErrorCode::OutOfMemory, "get");
    }
}

Result<int64_t> LibraryDb::count() noexcept {
    try {
        auto s = prepare(db_, "SELECT COUNT(*) FROM tracks");
        if (!s) return s.error();
        if (sqlite3_step(s.value().get()) != SQLITE_ROW) return dbError(db_, "count");
        return static_cast<int64_t>(sqlite3_column_int64(s.value().get(), 0));
    } catch (...) {
        return Result<int64_t>::fail(ErrorCode::OutOfMemory, "count");
    }
}

}  // namespace groove
