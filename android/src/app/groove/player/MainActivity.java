package app.groove.player;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.provider.DocumentsContract;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.webkit.ConsoleMessage;
import android.webkit.JavascriptInterface;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Hosts the Groove web app in a WebView. The app's files are bundled in assets/web and served
 * from https://appassets.androidplatform.net/ so modern web features (modules, workers) work.
 * Songs are picked with the system picker (files or whole folders), Groove keeps a persistent
 * read permission for them, and MediaLibrary streams them from storage — nothing is copied.
 */
public class MainActivity extends Activity {
    static final String HOST = "appassets.androidplatform.net";
    static final String START_URL = "https://" + HOST + "/index.html";
    private static final int PICK_FILES = 41;
    private static final int PICK_FOLDER = 42;
    private static final String TAG = "Groove";

    static volatile MainActivity current;

    private WebView web;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;
    private boolean hasFocus;
    private boolean pausedForFocus;
    private boolean playing;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        current = this;
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#3F8C5F")); // same as the splash, no white flash
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false); // songs are streamed by MediaLibrary, not read directly
        s.setTextZoom(100);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);

        web.addJavascriptInterface(new NativeBridge(), "GrooveNative");
        web.setWebViewClient(new AssetClient());
        web.setWebChromeClient(new Chrome());
        web.loadUrl(START_URL);
    }

    // ------------------------------------------------------------------ assets

    private class AssetClient extends WebViewClient {
        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            Uri url = request.getUrl();
            if (!HOST.equals(url.getHost())) {
                return error(403, "Forbidden");
            }
            String path = url.getPath();
            if ("/media".equals(path)) return MediaLibrary.serve(MainActivity.this, request);
            if (path == null || path.equals("/") || path.isEmpty()) path = "/index.html";
            if (path.contains("..")) return error(404, "Not Found");
            try {
                InputStream in = getAssets().open("web" + path);
                Map<String, String> headers = new HashMap<>();
                headers.put("Cache-Control", "no-cache");
                headers.put("X-Content-Type-Options", "nosniff");
                headers.put("Access-Control-Allow-Origin", "https://" + HOST);
                String mime = mimeFor(path);
                String enc = mime.startsWith("text/") || mime.contains("json") || mime.contains("javascript") || mime.contains("svg") ? "utf-8" : null;
                return new WebResourceResponse(mime, enc, 200, "OK", headers, in);
            } catch (IOException e) {
                return error(404, "Not Found");
            }
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            // Stay inside the app; nothing external is ever loaded.
            return !HOST.equals(request.getUrl().getHost());
        }

        @Override
        public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
            Log.w(TAG, "WebView renderer gone, restarting");
            recreate();
            return true;
        }
    }

    private static WebResourceResponse error(int code, String reason) {
        return new WebResourceResponse("text/plain", "utf-8", code, reason,
                new HashMap<String, String>(), new ByteArrayInputStream(new byte[0]));
    }

    private static String mimeFor(String path) {
        String p = path.toLowerCase();
        if (p.endsWith(".html")) return "text/html";
        if (p.endsWith(".js") || p.endsWith(".mjs")) return "text/javascript";
        if (p.endsWith(".css")) return "text/css";
        if (p.endsWith(".svg")) return "image/svg+xml";
        if (p.endsWith(".png")) return "image/png";
        if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
        if (p.endsWith(".woff2")) return "font/woff2";
        if (p.endsWith(".json")) return "application/json";
        if (p.endsWith(".webmanifest")) return "application/manifest+json";
        if (p.endsWith(".txt")) return "text/plain";
        return "application/octet-stream";
    }

    // ------------------------------------------------------------------ picking songs

    private class Chrome extends WebChromeClient {
        @Override
        public boolean onConsoleMessage(ConsoleMessage m) {
            Log.d(TAG, m.message() + " (" + m.sourceId() + ":" + m.lineNumber() + ")");
            return true;
        }
    }

    private void launchFilePicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                "audio/*", "application/ogg", "application/x-flac", "application/flac"
        });
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        start(i, PICK_FILES, "files");
    }

    private void launchFolderPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (Build.VERSION.SDK_INT >= 26) {
            // Start in the phone's Music folder.
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                    DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Music"));
        }
        start(i, PICK_FOLDER, "folder");
    }

    private void start(Intent i, int code, String type) {
        try {
            startActivityForResult(i, code);
        } catch (ActivityNotFoundException e) {
            JSONObject o = new JSONObject();
            try { o.put("type", type); o.put("error", "No file picker found on this phone."); } catch (Exception ignored) { }
            deliver(o);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, final Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FILES && requestCode != PICK_FOLDER) return;
        final boolean folder = requestCode == PICK_FOLDER;
        if (resultCode != RESULT_OK || data == null) {
            JSONObject o = new JSONObject();
            try { o.put("type", folder ? "folder" : "files"); o.put("cancelled", true); } catch (Exception ignored) { }
            deliver(o);
            return;
        }
        io.execute(new Runnable() {
            @Override public void run() {
                JSONObject o = new JSONObject();
                try {
                    if (folder) {
                        Uri tree = data.getData();
                        o.put("type", "folder");
                        MediaLibrary.persist(MainActivity.this, tree);
                        JSONObject f = new JSONObject();
                        f.put("uri", tree.toString());
                        f.put("name", MediaLibrary.folderName(MainActivity.this, tree));
                        o.put("folder", f);
                        o.put("items", MediaLibrary.scanTree(MainActivity.this, tree));
                    } else {
                        List<Uri> uris = new ArrayList<>();
                        ClipData clip = data.getClipData();
                        if (clip != null) {
                            for (int k = 0; k < clip.getItemCount(); k++) uris.add(clip.getItemAt(k).getUri());
                        } else if (data.getData() != null) {
                            uris.add(data.getData());
                        }
                        JSONArray items = new JSONArray();
                        for (Uri u : uris) {
                            MediaLibrary.allowForSession(u.toString());
                            JSONObject d = MediaLibrary.describe(MainActivity.this, u);
                            d.put("persisted", MediaLibrary.persist(MainActivity.this, u));
                            items.put(d);
                        }
                        o.put("type", "files");
                        o.put("items", items);
                    }
                    o.put("grants", MediaLibrary.grants(MainActivity.this).size());
                    o.put("grantLimit", MediaLibrary.grantLimit());
                } catch (Exception e) {
                    Log.w(TAG, "pick failed", e);
                    try { o.put("type", folder ? "folder" : "files"); o.put("error", String.valueOf(e.getMessage())); } catch (Exception ignored) { }
                }
                deliver(o);
            }
        });
    }

    /** Re-list saved folders so songs added to them since last time show up (and deleted ones go). */
    private void rescan(final String json) {
        io.execute(new Runnable() {
            @Override public void run() {
                JSONObject o = new JSONObject();
                try {
                    JSONArray in = new JSONArray(json);
                    JSONArray folders = new JSONArray();
                    for (int k = 0; k < in.length(); k++) {
                        String uri = in.getString(k);
                        JSONObject f = new JSONObject();
                        f.put("uri", uri);
                        try {
                            if (!MediaLibrary.grants(MainActivity.this).contains(uri)) throw new SecurityException("permission lost");
                            f.put("items", MediaLibrary.scanTree(MainActivity.this, Uri.parse(uri)));
                            f.put("ok", true);
                        } catch (Exception e) {
                            f.put("ok", false);
                            f.put("error", String.valueOf(e.getMessage()));
                        }
                        folders.put(f);
                    }
                    o.put("type", "rescan");
                    o.put("folders", folders);
                } catch (Exception e) {
                    try { o.put("type", "rescan"); o.put("error", String.valueOf(e.getMessage())); } catch (Exception ignored) { }
                }
                deliver(o);
            }
        });
    }

    /** Hand a result to the web app: window.grooveNativeResult(jsonString). */
    private void deliver(final JSONObject o) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (web != null) web.evaluateJavascript("window.grooveNativeResult && window.grooveNativeResult(" + JSONObject.quote(o.toString()) + ")", null);
            }
        });
    }

    // ------------------------------------------------------------------ JS bridge

    private class NativeBridge {
        @JavascriptInterface
        public void update(final String json) {
            runOnUiThread(new Runnable() {
                @Override public void run() { onPlaybackUpdate(json); }
            });
        }

        @JavascriptInterface
        public void theme(final String color, final boolean lightBars) {
            runOnUiThread(new Runnable() {
                @Override public void run() { applyBars(color, lightBars); }
            });
        }

        @JavascriptInterface
        public String platform() { return "android"; }

        @JavascriptInterface
        public void pickFiles() {
            runOnUiThread(new Runnable() { @Override public void run() { launchFilePicker(); } });
        }

        @JavascriptInterface
        public void pickFolder() {
            runOnUiThread(new Runnable() { @Override public void run() { launchFolderPicker(); } });
        }

        @JavascriptInterface
        public void rescanFolders(String json) { rescan(json); }

        /** The user removed a song or folder from Groove: give the permission back. */
        @JavascriptInterface
        public void release(String uri) { MediaLibrary.release(MainActivity.this, uri); }

        @JavascriptInterface
        public String grantInfo() {
            return "{\"grants\":" + MediaLibrary.grants(MainActivity.this).size() + ",\"limit\":" + MediaLibrary.grantLimit() + "}";
        }
    }

    private void onPlaybackUpdate(String json) {
        boolean hasTrack;
        try {
            JSONObject o = new JSONObject(json);
            hasTrack = o.optBoolean("hasTrack");
            playing = hasTrack && o.optBoolean("playing");
        } catch (Exception e) {
            return;
        }
        Intent svc = new Intent(this, PlaybackService.class);
        if (!hasTrack) {
            stopService(svc);
            abandonFocus();
            return;
        }
        svc.setAction(PlaybackService.ACTION_UPDATE);
        svc.putExtra(PlaybackService.EXTRA_JSON, json);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(svc); else startService(svc);
        if (playing) requestFocus();
    }

    private void applyBars(String color, boolean lightBars) {
        try {
            int c = Color.parseColor(color);
            Window w = getWindow();
            w.setStatusBarColor(c);
            w.setNavigationBarColor(c);
            web.setBackgroundColor(c);
            int flags = w.getDecorView().getSystemUiVisibility();
            int light = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            if (Build.VERSION.SDK_INT >= 26) light |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            flags = lightBars ? (flags | light) : (flags & ~light);
            w.getDecorView().setSystemUiVisibility(flags);
        } catch (IllegalArgumentException ignored) {
            // bad colour string from the page; keep the current one
        }
    }

    /** Send a command (play, pause, toggle, next, prev, seek:N) to the web app. */
    static void sendCommand(final String cmd) {
        final MainActivity a = current;
        if (a == null) return;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (a.web != null) a.web.evaluateJavascript("window.grooveRemote && window.grooveRemote(" + JSONObject.quote(cmd) + ")", null);
            }
        });
    }

    // ------------------------------------------------------------------ audio focus

    private final AudioManager.OnAudioFocusChangeListener focusListener = new AudioManager.OnAudioFocusChangeListener() {
        @Override
        public void onAudioFocusChange(int change) {
            if (change == AudioManager.AUDIOFOCUS_LOSS) {
                hasFocus = false;
                pausedForFocus = false;
                sendCommand("pause");
            } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
                if (playing) { pausedForFocus = true; sendCommand("pause"); }
            } else if (change == AudioManager.AUDIOFOCUS_GAIN) {
                hasFocus = true;
                if (pausedForFocus) { pausedForFocus = false; sendCommand("play"); }
            }
        }
    };

    private void requestFocus() {
        if (hasFocus) return;
        int r;
        if (Build.VERSION.SDK_INT >= 26) {
            if (focusRequest == null) {
                AudioAttributes attrs = new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build();
                focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(attrs)
                        .setOnAudioFocusChangeListener(focusListener)
                        .setWillPauseWhenDucked(false)
                        .build();
            }
            r = audioManager.requestAudioFocus(focusRequest);
        } else {
            r = audioManager.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        }
        hasFocus = r == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    private void abandonFocus() {
        if (!hasFocus) return;
        if (Build.VERSION.SDK_INT >= 26 && focusRequest != null) audioManager.abandonAudioFocusRequest(focusRequest);
        else audioManager.abandonAudioFocus(focusListener);
        hasFocus = false;
    }

    // ------------------------------------------------------------------ lifecycle

    @Override
    public void onBackPressed() {
        web.evaluateJavascript("(window.grooveBack && window.grooveBack()) ? 1 : 0", new ValueCallback<String>() {
            @Override
            public void onReceiveValue(String v) {
                // Nothing left to close: go to the home screen but keep the music playing.
                if (!"1".equals(v)) moveTaskToBack(true);
            }
        });
    }

    @Override
    protected void onDestroy() {
        if (current == this) current = null;
        stopService(new Intent(this, PlaybackService.class));
        abandonFocus();
        io.shutdownNow();
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
