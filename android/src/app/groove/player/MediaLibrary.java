package app.groove.player;

import android.content.ContentResolver;
import android.content.Context;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Songs the user picked stay where they are. Groove keeps Android's persistent read permission
 * for each picked file or folder and streams the bytes to the WebView on request, with proper
 * HTTP Range support so the player can seek without reading the whole file.
 *
 *   https://appassets.androidplatform.net/media?u=<content uri>
 */
final class MediaLibrary {
    private static final String TAG = "Groove";
    private static final Pattern AUDIO_EXT = Pattern.compile(
            "\\.(flac|m4a|alac|aac|mp4|aif|aiff|aifc|wav|wave|w64|mp3|ogg|oga|opus|dsf|dff)$", Pattern.CASE_INSENSITIVE);
    private static final int MAX_DEPTH = 16;
    private static final int MAX_FILES = 20000;

    private static volatile List<String> grants;
    /** Files picked this session whose app didn't allow a lasting permission (readable until Groove closes). */
    private static final java.util.Set<String> session = java.util.Collections.synchronizedSet(new java.util.HashSet<String>());
    private static volatile long grantsAt;

    private MediaLibrary() {}

    /** Android 10 keeps at most 128 persistent file permissions per app (512 from Android 11). */
    static int grantLimit() { return Build.VERSION.SDK_INT >= 30 ? 512 : 128; }

    static void invalidateGrants() { grantsAt = 0; }

    static List<String> grants(Context ctx) {
        long now = System.currentTimeMillis();
        List<String> g = grants;
        if (g == null || now - grantsAt > 5000) {
            g = new ArrayList<>();
            for (UriPermission p : ctx.getContentResolver().getPersistedUriPermissions()) {
                if (p.isReadPermission()) g.add(p.getUri().toString());
            }
            grants = g;
            grantsAt = now;
        }
        return g;
    }

    /** Only files the user picked (or files inside a folder they picked) can be read. */
    static void allowForSession(String uri) { session.add(uri); }

    static boolean isAllowed(Context ctx, String uri) {
        if (session.contains(uri)) return true;
        for (String g : grants(ctx)) {
            if (uri.equals(g)) return true;
            if (g.contains("/tree/") && uri.startsWith(g + "/document/")) return true;
        }
        return false;
    }

    static boolean isAudio(String name, String mime) {
        if (mime != null && (mime.startsWith("audio/") || mime.equals("application/ogg") || mime.equals("application/x-flac"))) return true;
        return name != null && AUDIO_EXT.matcher(name).find();
    }

    // ------------------------------------------------------------------ picking

    /** Take a persistent read permission so the file still opens after the app restarts. */
    static boolean persist(Context ctx, Uri uri) {
        try {
            ctx.getContentResolver().takePersistableUriPermission(uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
            invalidateGrants();
            return true;
        } catch (SecurityException e) {
            Log.w(TAG, "No persistable permission for " + uri);
            return false;
        }
    }

    static void release(Context ctx, String uri) {
        try {
            ctx.getContentResolver().releasePersistableUriPermission(Uri.parse(uri), android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
            // already gone
        }
        invalidateGrants();
    }

    /** Name, size and type of one picked file. */
    static JSONObject describe(Context ctx, Uri uri) {
        JSONObject o = new JSONObject();
        ContentResolver cr = ctx.getContentResolver();
        String name = null;
        long size = -1;
        Cursor c = null;
        try {
            c = cr.query(uri, new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE }, null, null, null);
            if (c != null && c.moveToFirst()) {
                name = c.getString(0);
                if (!c.isNull(1)) size = c.getLong(1);
            }
        } catch (Exception e) {
            Log.w(TAG, "query failed for " + uri, e);
        } finally {
            if (c != null) c.close();
        }
        try {
            o.put("uri", uri.toString());
            o.put("name", name != null ? name : uri.getLastPathSegment());
            o.put("size", size);
            o.put("mime", cr.getType(uri));
            o.put("path", "");
        } catch (Exception ignored) { }
        return o;
    }

    static String folderName(Context ctx, Uri tree) {
        try {
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            Cursor c = ctx.getContentResolver().query(doc, new String[] { DocumentsContract.Document.COLUMN_DISPLAY_NAME }, null, null, null);
            if (c != null) {
                try { if (c.moveToFirst()) return c.getString(0); } finally { c.close(); }
            }
        } catch (Exception ignored) { }
        String id = DocumentsContract.getTreeDocumentId(tree);
        int k = id.lastIndexOf(':');
        return k >= 0 && k < id.length() - 1 ? id.substring(k + 1) : id;
    }

    /** Every audio file inside a picked folder (and its sub-folders). */
    static JSONArray scanTree(Context ctx, Uri tree) throws Exception {
        JSONArray out = new JSONArray();
        walk(ctx.getContentResolver(), tree, DocumentsContract.getTreeDocumentId(tree), "", out, 0);
        return out;
    }

    private static void walk(ContentResolver cr, Uri tree, String docId, String path, JSONArray out, int depth) throws Exception {
        if (depth > MAX_DEPTH || out.length() >= MAX_FILES) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        Cursor c = cr.query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        }, null, null, null);
        if (c == null) {
            if (depth == 0) throw new IOException("Folder can't be read");
            return;
        }
        List<String[]> dirs = new ArrayList<>();
        try {
            while (c.moveToNext() && out.length() < MAX_FILES) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.startsWith(".")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    dirs.add(new String[] { id, name });
                } else if (isAudio(name, mime)) {
                    JSONObject o = new JSONObject();
                    o.put("uri", DocumentsContract.buildDocumentUriUsingTree(tree, id).toString());
                    o.put("name", name);
                    o.put("size", c.isNull(3) ? -1 : c.getLong(3));
                    o.put("mime", mime);
                    o.put("modified", c.isNull(4) ? 0 : c.getLong(4));
                    o.put("path", path);
                    out.put(o);
                }
            }
        } finally {
            c.close();
        }
        for (String[] d : dirs) walk(cr, tree, d[0], path.isEmpty() ? d[1] : path + "/" + d[1], out, depth + 1);
    }

    // ------------------------------------------------------------------ streaming

    static WebResourceResponse serve(Context ctx, WebResourceRequest request) {
        String u = request.getUrl().getQueryParameter("u");
        if (u == null || !u.startsWith("content://")) return plain(400, "Bad Request");
        if (!isAllowed(ctx, u)) return plain(403, "Forbidden");
        Uri uri = Uri.parse(u);
        ContentResolver cr = ctx.getContentResolver();

        String rangeHeader = null;
        Map<String, String> req = request.getRequestHeaders();
        if (req != null) {
            for (Map.Entry<String, String> e : req.entrySet()) {
                if ("range".equalsIgnoreCase(e.getKey())) rangeHeader = e.getValue();
            }
        }

        // Playback asks for decode=1: Groove's own decoders (libFLAC, libsndfile, DSD) turn the song into PCM
        // WAV here, so the WebView never has to demux/decode the original. Formats Groove doesn't decode
        // (MP3/AAC/ALAC/Ogg in this build) fall through to the original bytes below.
        if ("1".equals(request.getUrl().getQueryParameter("decode"))) {
            app.groove.engine.DecodedWavStream dec = app.groove.engine.DecodedWavStream.open(ctx, u);
            if (dec != null) {
                Map<String, String> h = new HashMap<>();
                h.put("Cache-Control", "no-store");
                h.put("X-Content-Type-Options", "nosniff");
                h.put("Accept-Ranges", "bytes");
                h.put("X-Groove-Decoder", "native");
                RangeSupport.Plan plan = RangeSupport.plan(rangeHeader, dec.size());
                if (plan.status == 416) {
                    closeQuietly(dec);
                    h.put("Content-Range", plan.contentRange());
                    return new WebResourceResponse("audio/wav", null, 416, "Range Not Satisfiable", h, new ByteArrayInputStream(new byte[0]));
                }
                InputStream body = new RangeSupport.Limited(dec, plan.streamLength());
                if (plan.status == 206) {
                    h.put("Content-Range", plan.contentRange());
                    return new WebResourceResponse("audio/wav", null, 206, "Partial Content", h, body);
                }
                return new WebResourceResponse("audio/wav", null, 200, "OK", h, body);
            }
        }

        ParcelFileDescriptor pfd;
        try {
            pfd = cr.openFileDescriptor(uri, "r");
            if (pfd == null) return plain(404, "Not Found");
        } catch (Exception e) {
            // Moved, deleted, or the SD card was removed.
            return plain(404, "Not Found");
        }
        long size = pfd.getStatSize();
        if (size < 0) size = describe(ctx, uri).optLong("size", -1);

        String mime = cr.getType(uri);
        if (mime == null || !(mime.startsWith("audio/") || mime.startsWith("application/ogg"))) mime = mimeFromName(u);

        Map<String, String> headers = new HashMap<>();
        headers.put("Cache-Control", "no-store");
        headers.put("X-Content-Type-Options", "nosniff");
        FileInputStream fis = new ParcelFileDescriptor.AutoCloseInputStream(pfd);

        if (size <= 0) {
            // Unknown length (rare): stream it whole; seeking won't be possible.
            return new WebResourceResponse(mime, null, 200, "OK", headers, fis);
        }
        // See RangeSupport: WebView skips to the start of the range itself, so the stream always
        // begins at byte 0, ends at the last requested byte, and we don't set Content-Length.
        RangeSupport.Plan plan = RangeSupport.plan(rangeHeader, size);
        headers.put("Accept-Ranges", "bytes");
        if (plan.status == 416) {
            closeQuietly(fis);
            headers.put("Content-Range", plan.contentRange());
            return new WebResourceResponse(mime, null, 416, "Range Not Satisfiable", headers, new ByteArrayInputStream(new byte[0]));
        }
        InputStream body = new RangeSupport.Limited(fis, plan.streamLength());
        if (plan.status == 206) {
            headers.put("Content-Range", plan.contentRange());
            return new WebResourceResponse(mime, null, 206, "Partial Content", headers, body);
        }
        return new WebResourceResponse(mime, null, 200, "OK", headers, body);
    }

    static String mimeFromName(String s) {
        String p = Uri.decode(s).toLowerCase(Locale.ROOT);
        if (p.endsWith(".flac")) return "audio/flac";
        if (p.endsWith(".mp3")) return "audio/mpeg";
        if (p.endsWith(".m4a") || p.endsWith(".mp4") || p.endsWith(".aac") || p.endsWith(".alac")) return "audio/mp4";
        if (p.endsWith(".wav") || p.endsWith(".wave")) return "audio/wav";
        if (p.endsWith(".ogg") || p.endsWith(".oga")) return "audio/ogg";
        if (p.endsWith(".opus")) return "audio/ogg";
        if (p.endsWith(".aif") || p.endsWith(".aiff") || p.endsWith(".aifc")) return "audio/aiff";
        return "application/octet-stream";
    }

    static WebResourceResponse plain(int code, String reason) {
        return new WebResourceResponse("text/plain", "utf-8", code, reason,
                new HashMap<String, String>(), new ByteArrayInputStream(new byte[0]));
    }

    private static void closeQuietly(java.io.Closeable c) {
        try { c.close(); } catch (IOException ignored) { }
    }
}
