package app.groove.engine

import java.lang.ref.WeakReference
import java.nio.ByteBuffer

/**
 * JNI entry points (libgroove.so). They are bound with RegisterNatives in JNI_OnLoad
 * (platform/android/jni/GrooveJni.cpp) using explicit signatures, so any mismatch between this file
 * and the C++ table fails at load time instead of crashing later. Keep both in sync.
 *
 * Handles are opaque ids into a native registry, never raw pointers: a stale or double-closed handle
 * is ignored by the native side.
 */
internal object GrooveNative {
    /** False when the native library isn't available (Android < 8.0, missing ABI): use the web player then. */
    val available: Boolean = try {
        System.loadLibrary("groove")
        true
    } catch (t: Throwable) {
        false
    }

    // ---- engine ----
    @JvmStatic external fun nativeCreate(opener: SafFileOpener, apiLevel: Int, mixerRate: Int, bitPerfectMixer: Boolean, ringSeconds: Double): Long
    @JvmStatic external fun nativeDestroy(handle: Long)
    @JvmStatic external fun nativeSetListener(handle: Long, listener: WeakReference<GrooveEngine>?)
    @JvmStatic external fun nativeLoad(handle: Long, trackId: Long, uri: String, autoplay: Boolean, startSeconds: Double)
    @JvmStatic external fun nativeSetNext(handle: Long, trackId: Long, uri: String?)
    @JvmStatic external fun nativePlay(handle: Long)
    @JvmStatic external fun nativePause(handle: Long)
    @JvmStatic external fun nativeStop(handle: Long)
    @JvmStatic external fun nativeSeek(handle: Long, seconds: Double)
    @JvmStatic external fun nativeSetVolume(handle: Long, linear: Double)
    @JvmStatic external fun nativeSetEq(handle: Long, enabled: Boolean, gainsDb: FloatArray, preampDb: Float, guaranteedHeadroom: Boolean)
    @JvmStatic external fun nativeSetDeviceInfo(handle: Long, mixerRate: Int, bitPerfectMixer: Boolean)
    @JvmStatic external fun nativeSnapshot(handle: Long, outLongs: LongArray, outDoubles: DoubleArray): Int
    @JvmStatic external fun nativeEnableTap(handle: Long, on: Boolean)
    @JvmStatic external fun nativeReadTap(handle: Long, directBuffer: ByteBuffer): Int

    // ---- library ----
    @JvmStatic external fun nativeLibraryOpen(dbPath: String, opener: SafFileOpener): Long
    @JvmStatic external fun nativeLibraryClose(handle: Long)
    @JvmStatic external fun nativeLibraryScan(
        handle: Long, rootKey: String, uris: Array<String>, folders: Array<String>,
        sizes: LongArray, mtimes: LongArray, progress: WeakReference<GrooveLibrary>?,
    ): Boolean
    @JvmStatic external fun nativeLibraryCancel(handle: Long)
    @JvmStatic external fun nativeLibraryQuery(
        handle: Long, matchAll: Boolean, fields: IntArray, ops: IntArray, texts: Array<String?>,
        numsA: DoubleArray, numsB: DoubleArray, sortField: Int, descending: Boolean, limit: Int, random: Boolean,
    ): String?
}
