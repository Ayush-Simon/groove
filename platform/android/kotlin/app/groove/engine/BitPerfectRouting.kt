package app.groove.engine

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioMixerAttributes
import android.os.Build
import android.annotation.TargetApi

/**
 * The only real bit-perfect path on Android: Android 14 (API 34) lets an app ask for a USB DAC to be driven
 * by a mixer in MIXER_BEHAVIOR_BIT_PERFECT mode at the track's own format. Older Android versions (including
 * Android 10) always run music through the system mixer, which converts to its own rate and mixes in float —
 * Groove then reports `osResamples = true` and never claims bit-perfect output.
 *
 * Call [enableForTrack] before loading a track whose rate differs from the current one; it reports what it did
 * to the engine via setDeviceInfo().
 */
@TargetApi(34) // every entry point also checks Build.VERSION at runtime
object BitPerfectRouting {
    private val musicAttributes: AudioAttributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    private fun usbOutput(am: AudioManager): AudioDeviceInfo? =
        am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }

    /** Returns true when a USB DAC is now in bit-perfect mode for [sampleRate]. */
    fun enableForTrack(context: Context, engine: GrooveEngine, sampleRate: Int): Boolean {
        if (Build.VERSION.SDK_INT < 34) return false
        val am = context.getSystemService(AudioManager::class.java) ?: return false
        val usb = usbOutput(am)
        if (usb == null) {
            engine.setDeviceInfo(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000, false)
            return false
        }
        val mixer = am.getSupportedMixerAttributes(usb).firstOrNull {
            it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT && it.format.sampleRate == sampleRate
        }
        val ok = mixer != null && am.setPreferredMixerAttributes(musicAttributes, usb, mixer)
        engine.setDeviceInfo(if (ok) sampleRate else 48000, ok)
        return ok
    }

    fun disable(context: Context, engine: GrooveEngine) {
        if (Build.VERSION.SDK_INT < 34) return
        val am = context.getSystemService(AudioManager::class.java) ?: return
        usbOutput(am)?.let { am.clearPreferredMixerAttributes(musicAttributes, it) }
        engine.setDeviceInfo(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000, false)
    }
}
