package app.groove.engine;

import java.lang.ref.WeakReference;
import java.nio.ByteBuffer;

/**
 * JNI entry points of libgroove.so. Bound in JNI_OnLoad with RegisterNatives and explicit signatures
 * (platform/android/jni/GrooveJni.cpp): a mismatch fails at load time, never as a crash later.
 * Handles are ids into a native registry, never raw pointers.
 * (platform/android/kotlin has the same API in Kotlin for a future Gradle build.)
 */
public final class GrooveNative {
    private GrooveNative() {}

    /** False if the library couldn't load (missing ABI, very old Android): the app keeps using the WebView decoders. */
    public static final boolean AVAILABLE;
    static {
        boolean ok;
        try {
            System.loadLibrary("groove");
            ok = true;
        } catch (Throwable t) {
            ok = false;
        }
        AVAILABLE = ok;
    }

    // engine (native AAudio playback — next phase)
    static native long nativeCreate(SafFileOpener opener, int apiLevel, int mixerRate, boolean bitPerfectMixer, double ringSeconds);
    static native void nativeDestroy(long handle);
    static native void nativeSetListener(long handle, WeakReference<GrooveEngine> listener);
    static native void nativeLoad(long handle, long trackId, String uri, boolean autoplay, double startSeconds);
    static native void nativeSetNext(long handle, long trackId, String uri);
    static native void nativePlay(long handle);
    static native void nativePause(long handle);
    static native void nativeStop(long handle);
    static native void nativeSeek(long handle, double seconds);
    static native void nativeSetVolume(long handle, double linear);
    static native void nativeSetEq(long handle, boolean enabled, float[] gainsDb, float preampDb, boolean guaranteedHeadroom);
    static native void nativeSetDeviceInfo(long handle, int mixerRate, boolean bitPerfectMixer);
    static native int nativeSnapshot(long handle, long[] outLongs, double[] outDoubles);
    static native void nativeEnableTap(long handle, boolean on);
    static native int nativeReadTap(long handle, ByteBuffer directBuffer);

    // library
    static native long nativeLibraryOpen(String dbPath, SafFileOpener opener);
    static native void nativeLibraryClose(long handle);
    static native boolean nativeLibraryScan(long handle, String rootKey, String[] uris, String[] folders, long[] sizes, long[] mtimes,
                                            WeakReference<GrooveLibrary> progress);
    static native void nativeLibraryCancel(long handle);
    static native String nativeLibraryQuery(long handle, boolean matchAll, int[] fields, int[] ops, String[] texts, double[] numsA,
                                            double[] numsB, int sortField, boolean descending, int limit, boolean random);

    // decode-to-WAV (used now: WebView playback goes through Groove's own decoders)
    static native long nativeDecodeOpen(int fd, String name);
    static native boolean nativeDecodeInfo(long handle, long[] out);
    static native int nativeDecodeRead(long handle, long offset, byte[] buf, int off, int len);
    static native void nativeDecodeClose(long handle);
}
