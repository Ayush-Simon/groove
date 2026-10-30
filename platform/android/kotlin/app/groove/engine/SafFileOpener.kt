package app.groove.engine

import android.content.Context
import android.net.Uri

/**
 * Opens content:// URIs (Storage Access Framework) for the native engine and scanner. Called from native
 * threads. The returned descriptor is detached from its ParcelFileDescriptor, so ownership passes to C++,
 * which wraps it in a scoped UniqueFd and closes it exactly once.
 */
class SafFileOpener(private val context: Context) {
    @Suppress("unused") // called from native code
    fun openFd(uri: String): Int = try {
        context.contentResolver.openFileDescriptor(Uri.parse(uri), "r")?.detachFd() ?: -1
    } catch (e: Exception) {
        // SecurityException (permission revoked), FileNotFoundException (moved/deleted, SD card removed) …
        -1
    }
}
