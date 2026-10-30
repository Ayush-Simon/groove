#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <new>
#include <system_error>

#include "groove/Library.h"

namespace fs = std::filesystem;

namespace groove {

namespace {
int64_t scanGeneration() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
constexpr int kBatch = 100;  // files per transaction
}  // namespace

LibraryScanner::LibraryScanner(std::string dbPath, std::shared_ptr<SourceOpener> opener)
    : dbPath_(std::move(dbPath)), opener_(opener ? std::move(opener) : std::make_shared<PathOpener>()) {}

LibraryScanner::~LibraryScanner() {
    cancel();
    wait();
}

void LibraryScanner::wait() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    if (worker_.joinable()) worker_.join();
}

bool LibraryScanner::isAudioPath(const std::string& name) noexcept {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kExt[] = {"flac", "wav", "wave", "aif", "aiff", "aifc", "w64", "caf", "ogg", "oga", "opus",
                                       "mp3", "m4a", "alac", "aac", "mp4", "dsf", "dff"};
    return std::any_of(std::begin(kExt), std::end(kExt), [&](const char* e) { return ext == e; });
}

Result<void> LibraryScanner::launch(std::function<void()> body) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_.load()) return Result<void>::fail(ErrorCode::InvalidState, "a scan is already running");
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    running_.store(true);
    try {
        worker_ = std::thread([this, body = std::move(body)]() noexcept {
            try {
                body();
            } catch (...) {
                // body reports its own errors through progress; nothing may escape the thread
            }
            running_.store(false);
        });
    } catch (...) {
        running_.store(false);
        return Result<void>::fail(ErrorCode::Internal, "couldn't start scanner thread");
    }
    return Result<void>::success();
}

void LibraryScanner::scanFile(LibraryDb& db, const std::string& path, const std::string& root, const std::string& folder,
                              const FileStamp& st, int64_t gen, ScanProgress& pr) {
    ++pr.seen;
    auto old = db.stamp(path);
    if (old && old.value() && old.value()->size == st.size && old.value()->mtime == st.mtime) {
        if (db.touch(path, gen)) ++pr.unchanged;
        return;  // incremental: unchanged files are not re-read
    }
    pr.current = path;
    auto src = opener_->open(path);
    if (!src) { ++pr.failed; return; }
    auto meta = readMetadata(*src.value());
    if (!meta) { ++pr.failed; return; }
    auto id = db.upsert(path, root, folder, st, meta.value(), gen);
    if (!id) { ++pr.failed; return; }
    if (old && old.value()) ++pr.updated;
    else ++pr.added;
}

Result<void> LibraryScanner::scanDirectories(std::vector<std::string> roots, ProgressFn progress) noexcept {
    try {
        return launch([this, roots = std::move(roots), progress = std::move(progress)]() {
            ScanProgress pr;
            auto dbr = LibraryDb::open(dbPath_);  // this thread's own connection
            if (!dbr) {
                pr.error = dbr.code();
                pr.finished = true;
                if (progress) progress(pr);
                return;
            }
            LibraryDb& db = *dbr.value();
            for (const std::string& root : roots) {
                const int64_t gen = scanGeneration();
                std::error_code ec;
                // error_code overloads only: a permission problem or a vanished directory is skipped, never thrown.
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                if (ec) { ++pr.failed; continue; }
                if (!db.begin()) { ++pr.failed; continue; }
                int inBatch = 0;
                bool rootComplete = true;
                for (; it != end; it.increment(ec)) {
                    if (ec) { rootComplete = false; ec.clear(); break; }  // drive removed mid-scan: keep the old index
                    if (cancel_.load()) { rootComplete = false; break; }
                    const fs::directory_entry& e = *it;
                    const std::string name = e.path().filename().string();
                    if (!name.empty() && name[0] == '.') {
                        if (e.is_directory(ec)) it.disable_recursion_pending();
                        continue;
                    }
                    if (!e.is_regular_file(ec) || !isAudioPath(name)) continue;
                    struct stat sb {};
                    if (::stat(e.path().c_str(), &sb) != 0) { ++pr.failed; continue; }
                    scanFile(db, e.path().string(), root, e.path().parent_path().string(), FileStamp{sb.st_size, static_cast<int64_t>(sb.st_mtime)}, gen, pr);
                    if (++inBatch >= kBatch) {
                        if (!db.commit() || !db.begin()) { rootComplete = false; break; }
                        inBatch = 0;
                        if (progress) progress(pr);
                    }
                }
                if (rootComplete) {
                    auto removed = db.sweep(root, gen);  // only after a complete walk: a partial one must not delete
                    if (removed) pr.removed += static_cast<uint64_t>(removed.value());
                }
                if (!db.commit()) db.rollback();
                if (cancel_.load()) break;
            }
            pr.finished = true;
            pr.cancelled = cancel_.load();
            pr.current.clear();
            if (progress) progress(pr);
        });
    } catch (...) {
        return Result<void>::fail(ErrorCode::OutOfMemory, "scanner");
    }
}

Result<void> LibraryScanner::scanEntries(std::string rootKey, std::vector<FileEntry> entries, ProgressFn progress) noexcept {
    try {
        return launch([this, rootKey = std::move(rootKey), entries = std::move(entries), progress = std::move(progress)]() {
            ScanProgress pr;
            auto dbr = LibraryDb::open(dbPath_);
            if (!dbr) {
                pr.error = dbr.code();
                pr.finished = true;
                if (progress) progress(pr);
                return;
            }
            LibraryDb& db = *dbr.value();
            const int64_t gen = scanGeneration();
            bool complete = true;
            if (db.begin()) {
                int inBatch = 0;
                for (const FileEntry& f : entries) {
                    if (cancel_.load()) { complete = false; break; }
                    scanFile(db, f.uri, rootKey, f.folder, FileStamp{f.size, f.mtime}, gen, pr);
                    if (++inBatch >= kBatch) {
                        if (!db.commit() || !db.begin()) { complete = false; break; }
                        inBatch = 0;
                        if (progress) progress(pr);
                    }
                }
                if (complete) {
                    auto removed = db.sweep(rootKey, gen);
                    if (removed) pr.removed += static_cast<uint64_t>(removed.value());
                }
                if (!db.commit()) db.rollback();
            } else {
                pr.error = ErrorCode::Io;
            }
            pr.finished = true;
            pr.cancelled = cancel_.load();
            pr.current.clear();
            if (progress) progress(pr);
        });
    } catch (...) {
        return Result<void>::fail(ErrorCode::OutOfMemory, "scanner");
    }
}

}  // namespace groove
