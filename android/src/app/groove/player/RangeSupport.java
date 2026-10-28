package app.groove.player;

import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * HTTP Range handling for responses returned from WebViewClient.shouldInterceptRequest.
 *
 * WebView does the range work itself (Chromium's InputStreamReader): it reads the total size
 * from InputStream.available(), skips the stream forward to the first requested byte, then reads
 * until end-of-stream, and sets Content-Length to the range length. So the stream we hand back
 * must START AT BYTE 0 of the file and END at the last requested byte, and available() must report
 * the exact remaining length. We only add the status line (206) and the Content-Range header.
 * (Pure Java so it can be tested off-device.)
 */
final class RangeSupport {
    private static final Pattern RANGE = Pattern.compile("bytes=(\\d*)-(\\d*)");

    private RangeSupport() {}

    /** A plan for one response: status 200/206/416, and the inclusive byte range to deliver. */
    static final class Plan {
        final int status;
        final long start, end, size;
        Plan(int status, long start, long end, long size) { this.status = status; this.start = start; this.end = end; this.size = size; }
        String contentRange() { return status == 416 ? "bytes */" + size : "bytes " + start + "-" + end + "/" + size; }
        /** How much of the file (from byte 0) the stream must contain. */
        long streamLength() { return end + 1; }
    }

    static Plan plan(String rangeHeader, long size) {
        if (rangeHeader == null || size <= 0) return new Plan(200, 0, size - 1, size);
        Matcher m = RANGE.matcher(rangeHeader.trim());
        if (!m.matches()) return new Plan(200, 0, size - 1, size); // multi-range etc.: send it all
        String a = m.group(1), b = m.group(2);
        long start, end = size - 1;
        if (a.isEmpty()) {
            if (b.isEmpty()) return new Plan(200, 0, size - 1, size);
            long n = Long.parseLong(b);
            if (n == 0) return new Plan(416, 0, 0, size);
            start = Math.max(0, size - n);                 // the last n bytes
        } else {
            start = Long.parseLong(a);
            if (!b.isEmpty()) end = Math.min(size - 1, Long.parseLong(b));
        }
        if (start >= size || start > end) return new Plan(416, 0, 0, size);
        return new Plan(206, start, end, size);
    }

    /** Returns at most `length` bytes from the start of `in`; available() is exact so WebView reads big chunks. */
    static final class Limited extends FilterInputStream {
        private long left;
        Limited(InputStream in, long length) { super(in); this.left = length; }
        @Override public int read() throws IOException {
            if (left <= 0) return -1;
            int r = super.read();
            if (r >= 0) left--;
            return r;
        }
        @Override public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) return -1;
            int r = super.read(b, off, (int) Math.min(len, left));
            if (r > 0) left -= r;
            return r;
        }
        @Override public long skip(long n) throws IOException {
            if (n <= 0 || left <= 0) return 0;
            long s = super.skip(Math.min(n, left));
            if (s > 0) left -= s;
            return s;
        }
        @Override public int available() {
            return (int) Math.min(Integer.MAX_VALUE, Math.max(0, left));
        }
    }
}
