package app.groove.engine;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.IOException;
import java.io.InputStream;

/**
 * A song decoded by Groove's own decoders (libFLAC, libsndfile, DSD→PCM), presented to the WebView as a
 * PCM WAV stream. The stream always starts at byte 0: WebView skips to the start of a Range itself
 * (see RangeSupport), and skip() here is free — it only moves the read position; decoding happens in read().
 */
public final class DecodedWavStream extends InputStream {
    private final long handle;
    private final long size;
    private long pos;
    private boolean closed;

    private DecodedWavStream(long handle, long size) { this.handle = handle; this.size = size; }

    /** Returns null if the native library is missing or can't decode this file (serve the original instead). */
    public static DecodedWavStream open(Context ctx, String uri) {
        if (!GrooveNative.AVAILABLE) return null;
        int fd;
        try {
            ParcelFileDescriptor pfd = ctx.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return null;
            fd = pfd.detachFd();
        } catch (Exception e) {
            return null;
        }
        long h = GrooveNative.nativeDecodeOpen(fd, uri);  // takes ownership of fd
        if (h == 0) return null;
        long[] info = new long[5];
        if (!GrooveNative.nativeDecodeInfo(h, info) || info[0] <= 0) {
            GrooveNative.nativeDecodeClose(h);
            return null;
        }
        return new DecodedWavStream(h, info[0]);
    }

    public long size() { return size; }

    @Override public int read() throws IOException {
        byte[] one = new byte[1];
        return read(one, 0, 1) == 1 ? one[0] & 0xFF : -1;
    }

    @Override public synchronized int read(byte[] b, int off, int len) throws IOException {
        if (closed) throw new IOException("closed");
        if (len == 0) return 0;
        if (pos >= size) return -1;
        int n = GrooveNative.nativeDecodeRead(handle, pos, b, off, (int) Math.min(len, size - pos));
        if (n < 0) throw new IOException("decode error (file moved or storage removed?)");
        if (n == 0) return -1;
        pos += n;
        return n;
    }

    @Override public synchronized long skip(long n) {
        if (n <= 0) return 0;
        long s = Math.min(n, size - pos);
        pos += s;
        return s;
    }

    @Override public synchronized int available() {
        return (int) Math.min(Integer.MAX_VALUE, Math.max(0, size - pos));
    }

    @Override public synchronized void close() {
        if (!closed) {
            closed = true;
            GrooveNative.nativeDecodeClose(handle);
        }
    }
}
