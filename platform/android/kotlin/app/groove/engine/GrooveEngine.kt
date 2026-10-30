package app.groove.engine

import android.content.Context
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.lang.ref.WeakReference
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Kotlin face of the C++ AudioEngine. Every call is non-blocking: it posts a command to the engine's
 * control thread, which runs commands strictly in order (the thread-safe state machine lives in C++).
 *
 * Memory rules:
 *  - The native side only ever sees this object through a WeakReference, so it can't keep it alive.
 *  - This object only holds its Listener weakly too: a Fragment/Activity/ViewModel that listens doesn't
 *    leak if it forgets to unregister. Keep a strong reference to your listener yourself.
 *  - Call close() when done (or use `use {}`); a second close() is a no-op.
 */
class GrooveEngine private constructor(private val handle: Long) : AutoCloseable {

    /** Mirrors groove::PlaybackState — same order. */
    enum class State { STOPPED, LOADING, PLAYING, PAUSED, ERROR }

    /** Mirrors groove::ErrorCode — same order. */
    enum class ErrorCode { NONE, FILE_NOT_FOUND, IO, UNSUPPORTED, CORRUPT, DEVICE_LOST, DEVICE_OPEN_FAILED, INVALID_ARGUMENT, INVALID_STATE, OUT_OF_MEMORY, INTERNAL }

    /** Mirrors groove::SampleFormat. */
    enum class SampleFormat { INT16, INT24_PACKED, INT32, FLOAT32 }

    data class OutputInfo(
        val sampleRate: Int,
        val format: SampleFormat,
        val exclusive: Boolean,
        val osResamples: Boolean,
        val bitPerfect: Boolean,
    )

    data class Snapshot(
        val state: State,
        val trackId: Long,
        val positionSeconds: Double,
        val durationSeconds: Double,
        val outputSampleRate: Int,
        val sourceSampleRate: Int,
        val sourceBits: Int,
        val bitPerfect: Boolean,
        val dspActive: Boolean,
        val exclusive: Boolean,
        val osResamples: Boolean,
        val fromDsd: Boolean,
        val underruns: Long,
        val clippedSamples: Long,
        val eqHeadroomDb: Double,
    )

    /** All callbacks arrive on the main thread. */
    interface Listener {
        fun onState(state: State, trackId: Long) {}
        fun onTrackStarted(trackId: Long) {}
        fun onTrackFailed(trackId: Long, error: ErrorCode, message: String) {}
        fun onQueueEnded() {}
        fun onOutput(info: OutputInfo) {}
    }

    private val closed = AtomicBoolean(false)
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var listenerRef: WeakReference<Listener>? = null
    private val snapLongs = LongArray(6)
    private val snapDoubles = DoubleArray(3)

    fun setListener(listener: Listener?) {
        listenerRef = listener?.let { WeakReference(it) }
    }

    fun load(trackId: Long, uri: String, autoplay: Boolean = true, startSeconds: Double = 0.0) {
        if (!closed.get()) GrooveNative.nativeLoad(handle, trackId, uri, autoplay, startSeconds)
    }

    /** Preload the track that follows (gapless). Call as soon as a track starts; null clears it. */
    fun setNext(trackId: Long, uri: String?) {
        if (!closed.get()) GrooveNative.nativeSetNext(handle, trackId, uri)
    }

    fun play() { if (!closed.get()) GrooveNative.nativePlay(handle) }
    fun pause() { if (!closed.get()) GrooveNative.nativePause(handle) }
    fun stop() { if (!closed.get()) GrooveNative.nativeStop(handle) }
    fun seek(seconds: Double) { if (!closed.get()) GrooveNative.nativeSeek(handle, seconds) }

    /** 1.0 = unity (bit-perfect path). Prefer hardware volume for bit-perfect listening. */
    fun setVolume(linear: Double) { if (!closed.get()) GrooveNative.nativeSetVolume(handle, linear) }

    /** gainsDb: 10 bands (31 Hz … 16 kHz). guaranteedHeadroom = mathematically clip-free (quieter boosts). */
    fun setEq(enabled: Boolean, gainsDb: FloatArray, preampDb: Float = 0f, guaranteedHeadroom: Boolean = false) {
        require(gainsDb.size == 10) { "10 bands expected" }
        if (!closed.get()) GrooveNative.nativeSetEq(handle, enabled, gainsDb, preampDb, guaranteedHeadroom)
    }

    /** Call when the output route changes (BitPerfectRouting does this for USB DACs on Android 14+). */
    fun setDeviceInfo(mixerSampleRate: Int, bitPerfectMixer: Boolean) {
        if (!closed.get()) GrooveNative.nativeSetDeviceInfo(handle, mixerSampleRate, bitPerfectMixer)
    }

    /** Cheap: fills preallocated arrays in native code. Safe from any thread. */
    fun snapshot(): Snapshot? = synchronized(snapLongs) {
        if (closed.get()) return null
        val s = GrooveNative.nativeSnapshot(handle, snapLongs, snapDoubles)
        if (s < 0) return null
        val flags = snapLongs[5]
        Snapshot(
            state = State.values().getOrElse(s) { State.ERROR },
            trackId = snapLongs[0],
            positionSeconds = snapDoubles[0],
            durationSeconds = snapDoubles[1],
            outputSampleRate = snapLongs[3].toInt(),
            sourceSampleRate = (snapLongs[4] ushr 16).toInt(),
            sourceBits = (snapLongs[4] and 0xFFFF).toInt(),
            bitPerfect = flags and 1L != 0L,
            dspActive = flags and 2L != 0L,
            exclusive = flags and 4L != 0L,
            osResamples = flags and 8L != 0L,
            fromDsd = flags and 16L != 0L,
            underruns = snapLongs[1],
            clippedSamples = snapLongs[2],
            eqHeadroomDb = snapDoubles[2],
        )
    }

    /**
     * Visualiser tap. Pass a buffer from [allocateTapBuffer] (direct, native byte order): native code copies
     * the newest float samples straight into it — no Java arrays, no GC pinning. Returns samples copied.
     */
    fun readTap(buffer: ByteBuffer): Int {
        require(buffer.isDirect) { "use allocateTapBuffer()" }
        if (closed.get()) return 0
        val n = GrooveNative.nativeReadTap(handle, buffer)
        buffer.clear().limit(n * 4)
        return n
    }

    fun enableTap(on: Boolean) { if (!closed.get()) GrooveNative.nativeEnableTap(handle, on) }

    override fun close() {
        if (closed.compareAndSet(false, true)) {
            GrooveNative.nativeSetListener(handle, null)
            GrooveNative.nativeDestroy(handle)  // stops audio and joins the engine threads
            main.removeCallbacksAndMessages(null)
        }
    }

    // ---- called by native code on the engine's control thread (names/signatures bound in JNI_OnLoad) ----
    @Suppress("unused") private fun onNativeState(state: Int, trackId: Long) =
        post { it.onState(State.values().getOrElse(state) { State.ERROR }, trackId) }

    @Suppress("unused") private fun onNativeTrackStarted(trackId: Long) = post { it.onTrackStarted(trackId) }

    @Suppress("unused") private fun onNativeTrackFailed(trackId: Long, code: Int, message: String) =
        post { it.onTrackFailed(trackId, ErrorCode.values().getOrElse(code) { ErrorCode.INTERNAL }, message) }

    @Suppress("unused") private fun onNativeQueueEnded() = post { it.onQueueEnded() }

    @Suppress("unused") private fun onNativeOutput(rate: Int, format: Int, exclusive: Boolean, osResamples: Boolean, bitPerfect: Boolean) =
        post { it.onOutput(OutputInfo(rate, SampleFormat.values().getOrElse(format) { SampleFormat.FLOAT32 }, exclusive, osResamples, bitPerfect)) }

    private inline fun post(crossinline f: (Listener) -> Unit) {
        // Never block the engine thread: just hand the event to the main thread.
        main.post { if (!closed.get()) listenerRef?.get()?.let(f) }
    }

    companion object {
        /** Returns null when the native engine can't run here; keep using the web player in that case. */
        fun create(context: Context, ringSeconds: Double = 2.0): GrooveEngine? {
            if (Build.VERSION.SDK_INT < 26 || !GrooveNative.available) return null
            val am = context.getSystemService(AudioManager::class.java)
            val mixerRate = am?.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000
            val handle = GrooveNative.nativeCreate(SafFileOpener(context.applicationContext), Build.VERSION.SDK_INT, mixerRate, false, ringSeconds)
            if (handle == 0L) return null
            val engine = GrooveEngine(handle)
            GrooveNative.nativeSetListener(handle, WeakReference(engine))
            return engine
        }

        fun allocateTapBuffer(samples: Int = 4096): ByteBuffer =
            ByteBuffer.allocateDirect(samples * 4).order(ByteOrder.nativeOrder())
    }
}
