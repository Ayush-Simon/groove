// JNI bridge: Kotlin (app.groove.engine.*) ↔ groove::AudioEngine / LibraryScanner.
//
// Safety rules enforced here:
//  * Handles, not pointers. Kotlin holds a jlong id into a registry of shared_ptrs. A stale, double-closed
//    or forged handle finds nothing and the call is a no-op — no use-after-free is possible from Kotlin.
//  * Every native method is registered in JNI_OnLoad with an explicit signature. A mismatch with the
//    Kotlin declaration fails loudly at System.loadLibrary(), not with a crash at the first call.
//  * PCM never crosses JNI during playback: decode → ring → AAudio all happen in native code. The only
//    PCM handed to Kotlin is the visualiser tap, copied into a DirectByteBuffer that Kotlin allocated once
//    (GetDirectBufferAddress: no GC pinning, no array copies, no critical sections).
//  * Callbacks into Kotlin go through a java.lang.ref.WeakReference, so the native side never keeps the
//    Kotlin object alive (no reference cycle across the GC boundary). Kotlin exceptions thrown inside a
//    callback are cleared; they never unwind into C++.
//  * Engine threads attach to the JVM once and detach automatically when they exit (pthread key).
#include <jni.h>
#include <android/log.h>
#include <pthread.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AAudioOutput.h"
#include "groove/AudioEngine.h"
#include "groove/Library.h"
#include "groove/WavStream.h"

using namespace groove;

// Android's jni.h uses `const char*` / `JNIEnv**` where the desktop JDK's uses `char*` / `void**`.
// GROOVE_DESKTOP_JNI is only defined by scripts/android-check/check.sh, which type-checks this file off-device.
#if defined(GROOVE_DESKTOP_JNI)
#define GROOVE_JSTR(s) const_cast<char*>(s)
#else
#define GROOVE_JSTR(s) s
#endif

namespace {

constexpr const char* kTag = "GrooveJNI";
JavaVM* gVm = nullptr;

struct Ids {
    jclass weakRefClass = nullptr;
    jmethodID weakGet = nullptr;
    jmethodID onState = nullptr, onTrackStarted = nullptr, onTrackFailed = nullptr, onQueueEnded = nullptr, onOutput = nullptr;
    jmethodID openFd = nullptr;
    jmethodID onScanProgress = nullptr;
} gIds;

// ---------------------------------------------------------------- thread attachment
pthread_key_t gDetachKey;
pthread_once_t gKeyOnce = PTHREAD_ONCE_INIT;
void makeKey() {
    pthread_key_create(&gDetachKey, [](void*) {
        if (gVm) gVm->DetachCurrentThread();  // runs when an engine/scanner thread exits
    });
}

/// JNIEnv for the current thread, attaching native threads on first use.
JNIEnv* env() {
    if (!gVm) return nullptr;
    JNIEnv* e = nullptr;
    const jint r = gVm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
    if (r == JNI_OK) return e;
    if (r != JNI_EDETACHED) return nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, GROOVE_JSTR("GrooveAudio"), nullptr};
#if defined(GROOVE_DESKTOP_JNI)
    if (gVm->AttachCurrentThread(reinterpret_cast<void**>(&e), &args) != JNI_OK) return nullptr;
#else
    if (gVm->AttachCurrentThread(&e, &args) != JNI_OK) return nullptr;
#endif
    pthread_once(&gKeyOnce, makeKey);
    pthread_setspecific(gDetachKey, reinterpret_cast<void*>(1));
    return e;
}

/// Local references created on long-lived native threads must be freed explicitly.
class LocalFrame {
public:
    LocalFrame(JNIEnv* e, jint capacity) : e_(e), ok_(e && e->PushLocalFrame(capacity) == 0) {}
    ~LocalFrame() { if (ok_) e_->PopLocalFrame(nullptr); }
    bool ok() const { return ok_; }
private:
    JNIEnv* e_;
    bool ok_;
};

bool clearException(JNIEnv* e) {
    if (e && e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
        return true;
    }
    return false;
}

/// Owns a JNI global reference.
class GlobalRef {
public:
    GlobalRef(JNIEnv* e, jobject o) : ref_(o ? e->NewGlobalRef(o) : nullptr) {}
    ~GlobalRef() { if (ref_) if (JNIEnv* e = env()) e->DeleteGlobalRef(ref_); }
    GlobalRef(const GlobalRef&) = delete;
    GlobalRef& operator=(const GlobalRef&) = delete;
    jobject get() const { return ref_; }
private:
    jobject ref_;
};

// ---------------------------------------------------------------- strings (UTF-8 ↔ UTF-16, not "modified UTF-8")
std::string fromJ(JNIEnv* e, jstring s) {
    if (!s) return {};
    const jsize n = e->GetStringLength(s);
    const jchar* c = e->GetStringChars(s, nullptr);
    if (!c) return {};
    std::string out;
    out.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) {
        uint32_t u = c[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n && c[i + 1] >= 0xDC00 && c[i + 1] < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (c[++i] - 0xDC00);
        if (u < 0x80) out += static_cast<char>(u);
        else if (u < 0x800) { out += static_cast<char>(0xC0 | u >> 6); out += static_cast<char>(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { out += static_cast<char>(0xE0 | u >> 12); out += static_cast<char>(0x80 | (u >> 6 & 0x3F)); out += static_cast<char>(0x80 | (u & 0x3F)); }
        else { out += static_cast<char>(0xF0 | u >> 18); out += static_cast<char>(0x80 | (u >> 12 & 0x3F)); out += static_cast<char>(0x80 | (u >> 6 & 0x3F)); out += static_cast<char>(0x80 | (u & 0x3F)); }
    }
    e->ReleaseStringChars(s, c);
    return out;
}

jstring toJ(JNIEnv* e, const std::string& s) {
    std::u16string w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        uint32_t u;
        int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + static_cast<size_t>(len) > s.size()) { w += u'�'; ++i; continue; }
        u = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k < len; ++k) u = (u << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(len);
        if (u >= 0x10000) { u -= 0x10000; w += static_cast<char16_t>(0xD800 + (u >> 10)); w += static_cast<char16_t>(0xDC00 + (u & 0x3FF)); }
        else w += static_cast<char16_t>(u);
    }
    return e->NewString(reinterpret_cast<const jchar*>(w.data()), static_cast<jsize>(w.size()));
}

// ---------------------------------------------------------------- handle registry
template <typename T>
class Registry {
public:
    jlong add(std::shared_ptr<T> v) {
        std::lock_guard<std::mutex> lk(m_);
        const jlong id = next_++;
        map_.emplace(id, std::move(v));
        return id;
    }
    std::shared_ptr<T> get(jlong id) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }
    std::shared_ptr<T> remove(jlong id) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(id);
        if (it == map_.end()) return nullptr;
        auto v = std::move(it->second);
        map_.erase(it);
        return v;  // destroyed by the caller, outside the lock
    }
private:
    std::mutex m_;
    std::unordered_map<jlong, std::shared_ptr<T>> map_;
    jlong next_ = 1;
};

// ---------------------------------------------------------------- SAF opener (Kotlin hands us a detached fd)
class JniOpener final : public SourceOpener {
public:
    JniOpener(JNIEnv* e, jobject opener) : opener_(e, opener) {}
    Result<std::unique_ptr<ByteSource>> open(const std::string& uri) noexcept override {
        using R = Result<std::unique_ptr<ByteSource>>;
        if (uri.rfind("content://", 0) != 0) {  // plain paths (app-private files, tests)
            auto f = FileSource::openPath(uri);
            if (!f) return f.error();
            return std::unique_ptr<ByteSource>(std::move(f).value());
        }
        JNIEnv* e = env();
        if (!e) return R::fail(ErrorCode::Internal, "no JNIEnv");
        LocalFrame frame(e, 4);
        jstring js = toJ(e, uri);
        const jint fd = js ? e->CallIntMethod(opener_.get(), gIds.openFd, js) : -1;
        if (clearException(e) || fd < 0) return R::fail(ErrorCode::FileNotFound, uri + ": permission revoked or file gone");
        auto f = FileSource::adopt(UniqueFd(fd), uri);  // we own the fd now; closed by FileSource
        if (!f) return f.error();
        return std::unique_ptr<ByteSource>(std::move(f).value());
    }
private:
    GlobalRef opener_;
};

// ---------------------------------------------------------------- engine listener → Kotlin (via WeakReference)
class JniListener final : public EngineListener {
public:
    JniListener(JNIEnv* e, jobject weakRef) : weak_(e, weakRef) {}
    void onStateChanged(PlaybackState s, uint64_t id) noexcept override {
        call([&](JNIEnv* e, jobject t) { e->CallVoidMethod(t, gIds.onState, static_cast<jint>(s), static_cast<jlong>(id)); });
    }
    void onTrackStarted(uint64_t id) noexcept override {
        call([&](JNIEnv* e, jobject t) { e->CallVoidMethod(t, gIds.onTrackStarted, static_cast<jlong>(id)); });
    }
    void onTrackFailed(uint64_t id, ErrorCode c, const std::string& msg) noexcept override {
        call([&](JNIEnv* e, jobject t) { e->CallVoidMethod(t, gIds.onTrackFailed, static_cast<jlong>(id), static_cast<jint>(c), toJ(e, msg)); });
    }
    void onQueueEnded() noexcept override {
        call([&](JNIEnv* e, jobject t) { e->CallVoidMethod(t, gIds.onQueueEnded); });
    }
    void onOutputChanged(const OutputConfig& c, bool bp) noexcept override {
        call([&](JNIEnv* e, jobject t) {
            e->CallVoidMethod(t, gIds.onOutput, static_cast<jint>(c.sampleRate), static_cast<jint>(c.format), static_cast<jboolean>(c.exclusive),
                              static_cast<jboolean>(c.osResamples), static_cast<jboolean>(bp));
        });
    }
private:
    template <typename F>
    void call(F&& f) noexcept {
        JNIEnv* e = env();
        if (!e) return;
        LocalFrame frame(e, 8);
        if (!frame.ok()) return;
        jobject target = e->CallObjectMethod(weak_.get(), gIds.weakGet);
        if (clearException(e) || !target) return;  // the Kotlin object was collected: nothing to tell
        f(e, target);
        clearException(e);
    }
    GlobalRef weak_;
};

struct EngineHolder {
    android::AAudioOutput* output = nullptr;  // owned by the engine
    std::shared_ptr<JniListener> listener;    // the engine only holds it weakly
    std::unique_ptr<AudioEngine> engine;
};

struct LibraryHolder {
    std::shared_ptr<SourceOpener> opener;
    std::unique_ptr<LibraryScanner> scanner;
    std::mutex dbMutex;                 // the UI connection is used from whichever Kotlin thread calls query()
    std::unique_ptr<LibraryDb> db;
    std::shared_ptr<GlobalRef> progress;
};

Registry<EngineHolder> gEngines;
Registry<LibraryHolder> gLibraries;

struct DecodeHolder {
    std::mutex m;  // one WebView request uses one stream, but never trust callers to serialise
    std::unique_ptr<WavStream> stream;
    std::vector<uint8_t> scratch;
};
Registry<DecodeHolder> gDecoders;

// ---------------------------------------------------------------- engine natives
jlong nativeCreate(JNIEnv* e, jclass, jobject opener, jint apiLevel, jint mixerRate, jboolean bitPerfectMixer, jdouble ringSeconds) {
    try {
        android::DeviceInfo info;
        info.apiLevel = apiLevel;
        info.mixerSampleRate = mixerRate > 0 ? static_cast<uint32_t>(mixerRate) : 48000u;
        info.bitPerfectMixer = bitPerfectMixer;
        auto out = std::make_unique<android::AAudioOutput>(info);
        auto holder = std::make_shared<EngineHolder>();
        holder->output = out.get();
        EngineConfig cfg;
        if (ringSeconds > 0) cfg.ringSeconds = ringSeconds;
        auto r = AudioEngine::create(cfg, std::move(out), std::make_shared<JniOpener>(e, opener));
        if (!r) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "engine create failed: %s", r.error().message.c_str());
            return 0;
        }
        holder->engine = std::move(r).value();
        return gEngines.add(std::move(holder));
    } catch (...) {
        return 0;
    }
}

void nativeDestroy(JNIEnv*, jclass, jlong h) {
    auto holder = gEngines.remove(h);
    holder.reset();  // joins the engine threads here, outside the registry lock
}

void nativeSetListener(JNIEnv* e, jclass, jlong h, jobject weakRef) {
    auto holder = gEngines.get(h);
    if (!holder) return;
    try {
        holder->listener = weakRef ? std::make_shared<JniListener>(e, weakRef) : nullptr;
        holder->engine->setListener(holder->listener);
    } catch (...) {
    }
}

void nativeLoad(JNIEnv* e, jclass, jlong h, jlong id, jstring uri, jboolean autoplay, jdouble start) {
    if (auto holder = gEngines.get(h)) {
        LoadOptions o;
        o.autoplay = autoplay;
        o.startSeconds = std::isfinite(start) ? start : 0.0;
        holder->engine->load(TrackSource{static_cast<uint64_t>(id), fromJ(e, uri)}, o);
    }
}

void nativeSetNext(JNIEnv* e, jclass, jlong h, jlong id, jstring uri) {
    if (auto holder = gEngines.get(h)) {
        if (uri) holder->engine->setNext(TrackSource{static_cast<uint64_t>(id), fromJ(e, uri)});
        else holder->engine->setNext(std::nullopt);
    }
}

void nativePlay(JNIEnv*, jclass, jlong h) { if (auto x = gEngines.get(h)) x->engine->play(); }
void nativePause(JNIEnv*, jclass, jlong h) { if (auto x = gEngines.get(h)) x->engine->pause(); }
void nativeStop(JNIEnv*, jclass, jlong h) { if (auto x = gEngines.get(h)) x->engine->stop(); }
void nativeSeek(JNIEnv*, jclass, jlong h, jdouble s) { if (auto x = gEngines.get(h)) x->engine->seek(s); }
void nativeSetVolume(JNIEnv*, jclass, jlong h, jdouble v) { if (auto x = gEngines.get(h)) x->engine->setVolume(v); }
void nativeEnableTap(JNIEnv*, jclass, jlong h, jboolean on) { if (auto x = gEngines.get(h)) x->engine->enableTap(on); }

void nativeSetEq(JNIEnv* e, jclass, jlong h, jboolean enabled, jfloatArray gains, jfloat preamp, jboolean guaranteed) {
    auto holder = gEngines.get(h);
    if (!holder || !gains || e->GetArrayLength(gains) != static_cast<jsize>(kEqBands)) return;
    jfloat g[kEqBands];
    e->GetFloatArrayRegion(gains, 0, static_cast<jsize>(kEqBands), g);  // copy, no pinning
    if (clearException(e)) return;
    EqSettings s;
    s.enabled = enabled;
    for (size_t i = 0; i < kEqBands; ++i) s.bands[i].gainDb = std::isfinite(g[i]) ? g[i] : 0.0;
    s.preampDb = std::isfinite(preamp) ? preamp : 0.0;
    s.headroom = guaranteed ? HeadroomMode::Guaranteed : HeadroomMode::PeakResponse;
    holder->engine->setEq(s);
}

void nativeSetDeviceInfo(JNIEnv*, jclass, jlong h, jint mixerRate, jboolean bitPerfectMixer) {
    if (auto x = gEngines.get(h)) x->output->setDeviceInfo(mixerRate > 0 ? static_cast<uint32_t>(mixerRate) : 48000u, bitPerfectMixer);
}

/// Fills Kotlin's preallocated arrays (no object allocation per call). Returns the PlaybackState ordinal.
jint nativeSnapshot(JNIEnv* e, jclass, jlong h, jlongArray outLongs, jdoubleArray outDoubles) {
    auto holder = gEngines.get(h);
    if (!holder || !outLongs || !outDoubles || e->GetArrayLength(outLongs) < 6 || e->GetArrayLength(outDoubles) < 3) return -1;
    const EngineSnapshot s = holder->engine->snapshot();
    const jlong flags = (s.bitPerfect ? 1 : 0) | (s.dspActive ? 2 : 0) | (s.output.exclusive ? 4 : 0) | (s.output.osResamples ? 8 : 0) |
                        (s.source.fromDsd ? 16 : 0);
    const jlong longs[6] = {static_cast<jlong>(s.trackId), static_cast<jlong>(s.underruns), static_cast<jlong>(s.clippedSamples),
                            static_cast<jlong>(s.output.sampleRate), static_cast<jlong>(s.source.sampleRate) << 16 | s.source.bitsPerSample, flags};
    const jdouble doubles[3] = {s.positionSeconds, s.durationSeconds, s.eqHeadroomDb};
    e->SetLongArrayRegion(outLongs, 0, 6, longs);
    e->SetDoubleArrayRegion(outDoubles, 0, 3, doubles);
    clearException(e);
    return static_cast<jint>(s.state);
}

/// Copies the newest output audio (float32, interleaved) into a direct ByteBuffer. Returns samples copied.
jint nativeReadTap(JNIEnv* e, jclass, jlong h, jobject directBuffer) {
    auto holder = gEngines.get(h);
    if (!holder || !directBuffer) return 0;
    void* addr = e->GetDirectBufferAddress(directBuffer);
    const jlong cap = e->GetDirectBufferCapacity(directBuffer);
    if (!addr || cap < static_cast<jlong>(sizeof(float))) return 0;  // not a direct buffer
    return static_cast<jint>(holder->engine->readTap(static_cast<float*>(addr), static_cast<size_t>(cap) / sizeof(float)));
}

// ---------------------------------------------------------------- library natives
jlong nativeLibraryOpen(JNIEnv* e, jclass, jstring dbPath, jobject opener) {
    try {
        auto holder = std::make_shared<LibraryHolder>();
        const std::string path = fromJ(e, dbPath);
        holder->opener = std::make_shared<JniOpener>(e, opener);
        auto db = LibraryDb::open(path);
        if (!db) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "library open failed: %s", db.error().message.c_str());
            return 0;
        }
        holder->db = std::move(db).value();
        holder->scanner = std::make_unique<LibraryScanner>(path, holder->opener);
        return gLibraries.add(std::move(holder));
    } catch (...) {
        return 0;
    }
}

void nativeLibraryClose(JNIEnv*, jclass, jlong h) {
    auto holder = gLibraries.remove(h);
    if (holder && holder->scanner) holder->scanner->cancel();
    holder.reset();  // joins the scanner thread
}

jboolean nativeLibraryScan(JNIEnv* e, jclass, jlong h, jstring rootKey, jobjectArray uris, jobjectArray folders, jlongArray sizes,
                           jlongArray mtimes, jobject progressWeak) {
    auto holder = gLibraries.get(h);
    if (!holder || !uris || !folders || !sizes || !mtimes) return JNI_FALSE;
    const jsize n = e->GetArrayLength(uris);
    if (e->GetArrayLength(folders) != n || e->GetArrayLength(sizes) != n || e->GetArrayLength(mtimes) != n) return JNI_FALSE;
    try {
        std::vector<FileEntry> entries(static_cast<size_t>(n));
        std::vector<jlong> sz(static_cast<size_t>(n)), mt(static_cast<size_t>(n));
        if (n > 0) {
            e->GetLongArrayRegion(sizes, 0, n, sz.data());
            e->GetLongArrayRegion(mtimes, 0, n, mt.data());
        }
        for (jsize i = 0; i < n; ++i) {
            LocalFrame frame(e, 4);
            auto& fe = entries[static_cast<size_t>(i)];
            fe.uri = fromJ(e, static_cast<jstring>(e->GetObjectArrayElement(uris, i)));
            fe.folder = fromJ(e, static_cast<jstring>(e->GetObjectArrayElement(folders, i)));
            fe.size = sz[static_cast<size_t>(i)];
            fe.mtime = mt[static_cast<size_t>(i)];
        }
        if (clearException(e)) return JNI_FALSE;
        holder->progress = progressWeak ? std::make_shared<GlobalRef>(e, progressWeak) : nullptr;
        std::weak_ptr<GlobalRef> weakProgress = holder->progress;
        auto r = holder->scanner->scanEntries(fromJ(e, rootKey), std::move(entries), [weakProgress](const ScanProgress& p) {
            auto ref = weakProgress.lock();
            JNIEnv* te = ref ? env() : nullptr;
            if (!te) return;
            LocalFrame frame(te, 4);
            jobject target = te->CallObjectMethod(ref->get(), gIds.weakGet);
            if (clearException(te) || !target) return;
            te->CallVoidMethod(target, gIds.onScanProgress, static_cast<jlong>(p.seen), static_cast<jlong>(p.added), static_cast<jlong>(p.updated),
                               static_cast<jlong>(p.removed), static_cast<jlong>(p.failed), static_cast<jboolean>(p.finished));
            clearException(te);
        });
        return r ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

void nativeLibraryCancel(JNIEnv*, jclass, jlong h) { if (auto x = gLibraries.get(h)) x->scanner->cancel(); }

void jsonEscape(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    o += '"';
}

/// Smart playlist query. Rules arrive as parallel arrays (field/op ordinals validated here); results return
/// as a JSON array string that Kotlin parses with org.json.
jstring nativeLibraryQuery(JNIEnv* e, jclass, jlong h, jboolean matchAll, jintArray fields, jintArray ops, jobjectArray texts,
                           jdoubleArray numsA, jdoubleArray numsB, jint sortField, jboolean desc, jint limit, jboolean random) {
    auto holder = gLibraries.get(h);
    if (!holder || !fields || !ops || !texts || !numsA || !numsB) return nullptr;
    const jsize n = e->GetArrayLength(fields);
    if (e->GetArrayLength(ops) != n || e->GetArrayLength(texts) != n || e->GetArrayLength(numsA) != n || e->GetArrayLength(numsB) != n) return nullptr;
    constexpr jint kMaxField = static_cast<jint>(Field::Lossless), kMaxOp = static_cast<jint>(Op::IsFalse);
    if (sortField < 0 || sortField > kMaxField) return nullptr;
    try {
        std::vector<jint> f(static_cast<size_t>(n)), o(static_cast<size_t>(n));
        std::vector<jdouble> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
        if (n > 0) {
            e->GetIntArrayRegion(fields, 0, n, f.data());
            e->GetIntArrayRegion(ops, 0, n, o.data());
            e->GetDoubleArrayRegion(numsA, 0, n, a.data());
            e->GetDoubleArrayRegion(numsB, 0, n, b.data());
        }
        SmartPlaylist p;
        p.matchAll = matchAll;
        p.sortBy = static_cast<Field>(sortField);
        p.descending = desc;
        p.limit = limit > 0 ? static_cast<uint32_t>(limit) : 0;
        p.random = random;
        for (jsize i = 0; i < n; ++i) {
            const auto k = static_cast<size_t>(i);
            if (f[k] < 0 || f[k] > kMaxField || o[k] < 0 || o[k] > kMaxOp) return nullptr;
            Rule r;
            r.field = static_cast<Field>(f[k]);
            r.op = static_cast<Op>(o[k]);
            LocalFrame frame(e, 2);
            auto js = static_cast<jstring>(e->GetObjectArrayElement(texts, i));
            if (js) r.a = fromJ(e, js);
            else r.a = a[k] == std::floor(a[k]) && std::fabs(a[k]) < 9e15 ? Value(static_cast<int64_t>(a[k])) : Value(a[k]);
            r.b = b[k] == std::floor(b[k]) && std::fabs(b[k]) < 9e15 ? Value(static_cast<int64_t>(b[k])) : Value(b[k]);
            p.rules.push_back(std::move(r));
        }
        if (clearException(e)) return nullptr;
        Result<std::vector<LibraryTrack>> rows = Result<std::vector<LibraryTrack>>::fail(ErrorCode::Internal);
        {
            std::lock_guard<std::mutex> lk(holder->dbMutex);
            rows = holder->db->query(p);
        }
        if (!rows) return nullptr;
        std::string json = "[";
        for (const LibraryTrack& t : rows.value()) {
            if (json.size() > 1) json += ',';
            json += "{\"id\":" + std::to_string(t.id) + ",\"uri\":";
            jsonEscape(json, t.path);
            json += ",\"title\":"; jsonEscape(json, t.title);
            json += ",\"artist\":"; jsonEscape(json, t.artist);
            json += ",\"album\":"; jsonEscape(json, t.album);
            json += ",\"albumArtist\":"; jsonEscape(json, t.albumArtist);
            json += ",\"genre\":"; jsonEscape(json, t.genre);
            json += ",\"codec\":"; jsonEscape(json, t.codec);
            json += ",\"container\":"; jsonEscape(json, t.container);
            json += ",\"year\":" + std::to_string(t.year) + ",\"trackNo\":" + std::to_string(t.trackNo) + ",\"discNo\":" + std::to_string(t.discNo) +
                    ",\"sampleRate\":" + std::to_string(t.sampleRate) + ",\"bitDepth\":" + std::to_string(t.bitDepth) +
                    ",\"channels\":" + std::to_string(t.channels) + ",\"duration\":" + std::to_string(t.durationSeconds) +
                    ",\"bitrate\":" + std::to_string(t.bitrateKbps) + ",\"lossless\":" + (t.lossless ? "true" : "false") +
                    ",\"hasArtwork\":" + (t.hasArtwork ? "true" : "false") + "}";
        }
        json += "]";
        return toJ(e, json);
    } catch (...) {
        return nullptr;
    }
}

// ---------------------------------------------------------------- decode-to-WAV (WebView playback through Groove's decoders)
/// Takes ownership of `fd` in every case. Returns 0 if Groove can't decode the file (the caller then serves it raw).
jlong nativeDecodeOpen(JNIEnv* e, jclass, jint fd, jstring name) {
    UniqueFd owned(fd);
    try {
        auto src = FileSource::adopt(std::move(owned), fromJ(e, name));
        if (!src) return 0;
        auto w = WavStream::open(std::move(src).value());
        if (!w) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "native decode not used: %s", w.error().message.c_str());
            return 0;
        }
        auto h = std::make_shared<DecodeHolder>();
        h->stream = std::move(w).value();
        h->scratch.resize(64 * 1024);
        return gDecoders.add(std::move(h));
    } catch (...) {
        return 0;
    }
}

/// out: [totalBytes, sampleRate, channels, outBits, frames]
jboolean nativeDecodeInfo(JNIEnv* e, jclass, jlong h, jlongArray out) {
    auto d = gDecoders.get(h);
    if (!d || !out || e->GetArrayLength(out) < 5) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(d->m);
    const jlong v[5] = {static_cast<jlong>(d->stream->size()), static_cast<jlong>(d->stream->format().sampleRate),
                        static_cast<jlong>(d->stream->format().channels), static_cast<jlong>(d->stream->outBits()),
                        static_cast<jlong>(d->stream->frames())};
    e->SetLongArrayRegion(out, 0, 5, v);
    return clearException(e) ? JNI_FALSE : JNI_TRUE;
}

/// Reads up to len bytes at `offset` into buf[off…]. Returns bytes read, 0 at the end, -1 on a read error.
jint nativeDecodeRead(JNIEnv* e, jclass, jlong h, jlong offset, jbyteArray buf, jint off, jint len) {
    auto d = gDecoders.get(h);
    if (!d || !buf || offset < 0 || off < 0 || len < 0 || off > e->GetArrayLength(buf) - len) return -1;
    std::lock_guard<std::mutex> lk(d->m);
    jint done = 0;
    while (done < len) {
        const size_t want = std::min<size_t>(d->scratch.size(), static_cast<size_t>(len - done));
        auto r = d->stream->read(static_cast<uint64_t>(offset) + static_cast<uint64_t>(done), d->scratch.data(), want);
        if (!r) return done > 0 ? done : -1;
        if (r.value() == 0) break;
        e->SetByteArrayRegion(buf, off + done, static_cast<jsize>(r.value()), reinterpret_cast<const jbyte*>(d->scratch.data()));  // copy, no pinning
        if (clearException(e)) return -1;
        done += static_cast<jint>(r.value());
    }
    return done;
}

void nativeDecodeClose(JNIEnv*, jclass, jlong h) {
    auto d = gDecoders.remove(h);
    d.reset();
}

// ---------------------------------------------------------------- registration
const JNINativeMethod kNativeMethods[] = {
    {GROOVE_JSTR("nativeCreate"), GROOVE_JSTR("(Lapp/groove/engine/SafFileOpener;IIZD)J"), reinterpret_cast<void*>(nativeCreate)},
    {GROOVE_JSTR("nativeDestroy"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativeDestroy)},
    {GROOVE_JSTR("nativeSetListener"), GROOVE_JSTR("(JLjava/lang/ref/WeakReference;)V"), reinterpret_cast<void*>(nativeSetListener)},
    {GROOVE_JSTR("nativeLoad"), GROOVE_JSTR("(JJLjava/lang/String;ZD)V"), reinterpret_cast<void*>(nativeLoad)},
    {GROOVE_JSTR("nativeSetNext"), GROOVE_JSTR("(JJLjava/lang/String;)V"), reinterpret_cast<void*>(nativeSetNext)},
    {GROOVE_JSTR("nativePlay"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativePlay)},
    {GROOVE_JSTR("nativePause"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativePause)},
    {GROOVE_JSTR("nativeStop"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativeStop)},
    {GROOVE_JSTR("nativeSeek"), GROOVE_JSTR("(JD)V"), reinterpret_cast<void*>(nativeSeek)},
    {GROOVE_JSTR("nativeSetVolume"), GROOVE_JSTR("(JD)V"), reinterpret_cast<void*>(nativeSetVolume)},
    {GROOVE_JSTR("nativeSetEq"), GROOVE_JSTR("(JZ[FFZ)V"), reinterpret_cast<void*>(nativeSetEq)},
    {GROOVE_JSTR("nativeSetDeviceInfo"), GROOVE_JSTR("(JIZ)V"), reinterpret_cast<void*>(nativeSetDeviceInfo)},
    {GROOVE_JSTR("nativeSnapshot"), GROOVE_JSTR("(J[J[D)I"), reinterpret_cast<void*>(nativeSnapshot)},
    {GROOVE_JSTR("nativeEnableTap"), GROOVE_JSTR("(JZ)V"), reinterpret_cast<void*>(nativeEnableTap)},
    {GROOVE_JSTR("nativeReadTap"), GROOVE_JSTR("(JLjava/nio/ByteBuffer;)I"), reinterpret_cast<void*>(nativeReadTap)},
    {GROOVE_JSTR("nativeLibraryOpen"), GROOVE_JSTR("(Ljava/lang/String;Lapp/groove/engine/SafFileOpener;)J"), reinterpret_cast<void*>(nativeLibraryOpen)},
    {GROOVE_JSTR("nativeLibraryClose"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativeLibraryClose)},
    {GROOVE_JSTR("nativeLibraryScan"), GROOVE_JSTR("(JLjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[J[JLjava/lang/ref/WeakReference;)Z"),
     reinterpret_cast<void*>(nativeLibraryScan)},
    {GROOVE_JSTR("nativeLibraryCancel"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativeLibraryCancel)},
    {GROOVE_JSTR("nativeDecodeOpen"), GROOVE_JSTR("(ILjava/lang/String;)J"), reinterpret_cast<void*>(nativeDecodeOpen)},
    {GROOVE_JSTR("nativeDecodeInfo"), GROOVE_JSTR("(J[J)Z"), reinterpret_cast<void*>(nativeDecodeInfo)},
    {GROOVE_JSTR("nativeDecodeRead"), GROOVE_JSTR("(JJ[BII)I"), reinterpret_cast<void*>(nativeDecodeRead)},
    {GROOVE_JSTR("nativeDecodeClose"), GROOVE_JSTR("(J)V"), reinterpret_cast<void*>(nativeDecodeClose)},
    {GROOVE_JSTR("nativeLibraryQuery"), GROOVE_JSTR("(JZ[I[I[Ljava/lang/String;[D[DIZIZ)Ljava/lang/String;"), reinterpret_cast<void*>(nativeLibraryQuery)},
};

bool cacheMethod(JNIEnv* e, jmethodID& out, const char* cls, const char* name, const char* sig) {
    jclass c = e->FindClass(cls);
    if (!c) return false;
    out = e->GetMethodID(c, name, sig);
    e->DeleteLocalRef(c);
    return out != nullptr;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass weak = e->FindClass("java/lang/ref/WeakReference");
    if (!weak) return JNI_ERR;
    gIds.weakRefClass = static_cast<jclass>(e->NewGlobalRef(weak));
    gIds.weakGet = e->GetMethodID(weak, "get", "()Ljava/lang/Object;");
    e->DeleteLocalRef(weak);
    const char* engine = "app/groove/engine/GrooveEngine";
    const bool ok = gIds.weakGet &&
                    cacheMethod(e, gIds.onState, engine, "onNativeState", "(IJ)V") &&
                    cacheMethod(e, gIds.onTrackStarted, engine, "onNativeTrackStarted", "(J)V") &&
                    cacheMethod(e, gIds.onTrackFailed, engine, "onNativeTrackFailed", "(JILjava/lang/String;)V") &&
                    cacheMethod(e, gIds.onQueueEnded, engine, "onNativeQueueEnded", "()V") &&
                    cacheMethod(e, gIds.onOutput, engine, "onNativeOutput", "(IIZZZ)V") &&
                    cacheMethod(e, gIds.openFd, "app/groove/engine/SafFileOpener", "openFd", "(Ljava/lang/String;)I") &&
                    cacheMethod(e, gIds.onScanProgress, "app/groove/engine/GrooveLibrary", "onNativeScanProgress", "(JJJJJZ)V");
    if (!ok) {
        clearException(e);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Kotlin callback signatures don't match the native bridge");
        return JNI_ERR;
    }
    jclass natives = e->FindClass("app/groove/engine/GrooveNative");
    if (!natives) return JNI_ERR;
    const jint rc = e->RegisterNatives(natives, kNativeMethods, sizeof kNativeMethods / sizeof *kNativeMethods);
    e->DeleteLocalRef(natives);
    if (rc != JNI_OK) {
        clearException(e);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "RegisterNatives failed: Kotlin and C++ declarations differ");
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
