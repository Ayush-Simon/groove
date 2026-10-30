package app.groove.engine;

import android.content.Context;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.lang.ref.WeakReference;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Java face of the C++ AudioEngine (native AAudio playback). Not used by the UI yet; kept complete so the
 * native callbacks bound in JNI_OnLoad exist. Callbacks arrive on the main thread; the listener is held weakly.
 */
public final class GrooveEngine implements AutoCloseable {
    public interface Listener {
        void onState(int state, long trackId);
        void onTrackStarted(long trackId);
        void onTrackFailed(long trackId, int errorCode, String message);
        void onQueueEnded();
        void onOutput(int sampleRate, int format, boolean exclusive, boolean osResamples, boolean bitPerfect);
    }

    private final long handle;
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile WeakReference<Listener> listener;

    private GrooveEngine(long handle) { this.handle = handle; }

    public static GrooveEngine create(Context context) {
        if (Build.VERSION.SDK_INT < 26 || !GrooveNative.AVAILABLE) return null;
        AudioManager am = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        int rate = 48000;
        try { rate = Integer.parseInt(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)); } catch (Exception ignored) { }
        long h = GrooveNative.nativeCreate(new SafFileOpener(context), Build.VERSION.SDK_INT, rate, false, 2.0);
        if (h == 0) return null;
        GrooveEngine e = new GrooveEngine(h);
        GrooveNative.nativeSetListener(h, new WeakReference<GrooveEngine>(e));
        return e;
    }

    public void setListener(Listener l) { listener = l == null ? null : new WeakReference<Listener>(l); }
    public void load(long id, String uri, boolean autoplay, double start) { if (!closed.get()) GrooveNative.nativeLoad(handle, id, uri, autoplay, start); }
    public void setNext(long id, String uri) { if (!closed.get()) GrooveNative.nativeSetNext(handle, id, uri); }
    public void play() { if (!closed.get()) GrooveNative.nativePlay(handle); }
    public void pause() { if (!closed.get()) GrooveNative.nativePause(handle); }
    public void stop() { if (!closed.get()) GrooveNative.nativeStop(handle); }
    public void seek(double s) { if (!closed.get()) GrooveNative.nativeSeek(handle, s); }

    @Override public void close() {
        if (closed.compareAndSet(false, true)) {
            GrooveNative.nativeSetListener(handle, null);
            GrooveNative.nativeDestroy(handle);
            main.removeCallbacksAndMessages(null);
        }
    }

    private Listener target() {
        WeakReference<Listener> r = listener;
        return r == null || closed.get() ? null : r.get();
    }

    // Called by native code on the engine thread (names/signatures bound in JNI_OnLoad); never block here.
    private void onNativeState(final int state, final long trackId) {
        main.post(new Runnable() { @Override public void run() { Listener l = target(); if (l != null) l.onState(state, trackId); } });
    }
    private void onNativeTrackStarted(final long trackId) {
        main.post(new Runnable() { @Override public void run() { Listener l = target(); if (l != null) l.onTrackStarted(trackId); } });
    }
    private void onNativeTrackFailed(final long trackId, final int code, final String message) {
        main.post(new Runnable() { @Override public void run() { Listener l = target(); if (l != null) l.onTrackFailed(trackId, code, message); } });
    }
    private void onNativeQueueEnded() {
        main.post(new Runnable() { @Override public void run() { Listener l = target(); if (l != null) l.onQueueEnded(); } });
    }
    private void onNativeOutput(final int rate, final int format, final boolean exclusive, final boolean osResamples, final boolean bitPerfect) {
        main.post(new Runnable() { @Override public void run() { Listener l = target(); if (l != null) l.onOutput(rate, format, exclusive, osResamples, bitPerfect); } });
    }
}
