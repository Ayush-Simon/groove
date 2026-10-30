// Library index (SQLite) + asynchronous storage scanner + smart playlists.
//
// Threading: SQLite connections are not shared between threads. The scanner opens its own connection on
// its worker thread; the UI side opens another for queries. WAL mode lets them run at the same time, so a
// scan never blocks the UI (no ANR / watchdog kills).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "groove/ByteSource.h"
#include "groove/Metadata.h"
#include "groove/Result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace groove {

struct LibraryTrack {
    int64_t id = 0;
    std::string path, root, folder;
    std::string title, artist, album, albumArtist, genre, composer, container, codec;
    int year = 0, trackNo = 0, discNo = 0;
    uint32_t sampleRate = 0, bitrateKbps = 0;
    uint16_t bitDepth = 0, channels = 0;
    double durationSeconds = 0;
    bool lossless = false, hasArtwork = false;
    int64_t fileSize = 0, mtime = 0, addedAt = 0;
};

// ---------------------------------------------------------------- smart playlists
enum class Field : uint8_t {
    Title, Artist, Album, AlbumArtist, Genre, Composer, Container, Codec, Folder, Path,         // text
    Year, TrackNo, DiscNo, SampleRate, BitDepth, Channels, Duration, Bitrate, AddedAt,         // numeric
    Lossless,                                                                                   // boolean
};
enum class Op : uint8_t { Equals, NotEquals, Contains, NotContains, StartsWith, Greater, GreaterOrEqual, Less, LessOrEqual, Between, IsTrue, IsFalse };
using Value = std::variant<std::monostate, int64_t, double, std::string>;

struct Rule {
    Field field = Field::Title;
    Op op = Op::Contains;
    Value a{}, b{};
};

struct SmartPlaylist {
    bool matchAll = true;          // AND (true) or OR (false)
    std::vector<Rule> rules;       // empty = whole library
    Field sortBy = Field::Artist;
    bool descending = false;
    uint32_t limit = 0;            // 0 = no limit
    bool random = false;           // ORDER BY random() (ignores sortBy)
};

struct BuiltQuery {
    std::string sql;               // only whitelisted column names and operators; every value is a bound parameter
    std::vector<Value> params;
};

/// Validates the rules (field/operator/value types) and builds a parameterised query.
Result<BuiltQuery> buildSmartQuery(const SmartPlaylist& p) noexcept;

// ---------------------------------------------------------------- database
struct FileStamp { int64_t size = 0; int64_t mtime = 0; };

class LibraryDb {
    struct PrivateTag { explicit PrivateTag() = default; };  // only LibraryDb can name this type

public:
    /// Opens (creating/migrating) the index. One instance per thread.
    static Result<std::unique_ptr<LibraryDb>> open(const std::string& path) noexcept;
    LibraryDb(PrivateTag, sqlite3* db) noexcept;
    ~LibraryDb();
    LibraryDb(const LibraryDb&) = delete;
    LibraryDb& operator=(const LibraryDb&) = delete;

    Result<void> begin() noexcept;
    Result<void> commit() noexcept;
    void rollback() noexcept;

    Result<std::optional<FileStamp>> stamp(const std::string& path) noexcept;
    Result<void> touch(const std::string& path, int64_t scanGen) noexcept;
    Result<int64_t> upsert(const std::string& path, const std::string& root, const std::string& folder, const FileStamp& st,
                           const TrackMetadata& m, int64_t scanGen) noexcept;
    /// Removes tracks under `root` that the latest scan didn't see. Returns how many.
    Result<int64_t> sweep(const std::string& root, int64_t scanGen) noexcept;
    Result<void> remove(int64_t id) noexcept;

    Result<std::vector<LibraryTrack>> query(const SmartPlaylist& p) noexcept;
    Result<std::optional<LibraryTrack>> get(int64_t id) noexcept;
    Result<int64_t> count() noexcept;
    static int schemaVersion() noexcept;

private:
    Result<void> exec(const char* sql) noexcept;
    sqlite3* db_;
};

// ---------------------------------------------------------------- scanner
struct ScanProgress {
    uint64_t seen = 0, added = 0, updated = 0, unchanged = 0, removed = 0, failed = 0;
    std::string current;     // file being read
    bool finished = false, cancelled = false;
    ErrorCode error = ErrorCode::None;
};

/// A file listed by the platform (Android: DocumentsContract rows for a SAF tree).
struct FileEntry {
    std::string uri, folder;
    int64_t size = 0, mtime = 0;
};

class LibraryScanner {
public:
    using ProgressFn = std::function<void(const ScanProgress&)>;  // called on the scanner thread

    LibraryScanner(std::string dbPath, std::shared_ptr<SourceOpener> opener);
    ~LibraryScanner();  // cancels and joins
    LibraryScanner(const LibraryScanner&) = delete;
    LibraryScanner& operator=(const LibraryScanner&) = delete;

    /// Desktop / iOS: walk directories with std::filesystem.
    Result<void> scanDirectories(std::vector<std::string> roots, ProgressFn progress) noexcept;
    /// Android: the platform lists the files; the scanner reads tags through the SourceOpener.
    Result<void> scanEntries(std::string rootKey, std::vector<FileEntry> entries, ProgressFn progress) noexcept;

    void cancel() noexcept { cancel_.store(true); }
    bool running() const noexcept { return running_.load(); }
    void wait() noexcept;

    static bool isAudioPath(const std::string& name) noexcept;

private:
    Result<void> launch(std::function<void()> body) noexcept;
    void scanFile(LibraryDb& db, const std::string& path, const std::string& root, const std::string& folder, const FileStamp& st,
                  int64_t gen, ScanProgress& pr);

    std::string dbPath_;
    std::shared_ptr<SourceOpener> opener_;
    std::thread worker_;
    std::mutex mutex_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
};

}  // namespace groove
