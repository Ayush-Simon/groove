package app.groove.engine;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

/** Opens content:// URIs for native threads; the detached fd is owned (and closed) by C++. */
public final class SafFileOpener {
    private final Context context;

    public SafFileOpener(Context context) { this.context = context.getApplicationContext(); }

    public int openFd(String uri) {
        try {
            ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return pfd != null ? pfd.detachFd() : -1;
        } catch (Exception e) {
            return -1;  // permission revoked, moved/deleted, SD card removed
        }
    }
}
