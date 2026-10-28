package app.groove.player;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Base64;

import org.json.JSONObject;

/**
 * Keeps music playing with the screen off: a foreground service with a media notification,
 * lock-screen controls and headphone / Bluetooth buttons (MediaSession).
 */
public class PlaybackService extends Service {
    static final String ACTION_UPDATE = "app.groove.player.UPDATE";
    static final String ACTION_TOGGLE = "app.groove.player.TOGGLE";
    static final String ACTION_NEXT = "app.groove.player.NEXT";
    static final String ACTION_PREV = "app.groove.player.PREV";
    static final String ACTION_CLOSE = "app.groove.player.CLOSE";
    static final String EXTRA_JSON = "json";
    private static final String CHANNEL = "playback";
    private static final int NOTIFICATION_ID = 7;

    private MediaSession session;
    private String title = "", artist = "", album = "";
    private boolean playing;
    private Bitmap art;
    private boolean noisyRegistered;

    private final BroadcastReceiver noisy = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // Headphones unplugged / Bluetooth disconnected: pause instead of blasting the speaker.
            if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction())) MainActivity.sendCommand("pause");
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            ch.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
            ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).createNotificationChannel(ch);
        }
        session = new MediaSession(this, "Groove");
        session.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { MainActivity.sendCommand("play"); }
            @Override public void onPause() { MainActivity.sendCommand("pause"); }
            @Override public void onStop() { MainActivity.sendCommand("pause"); }
            @Override public void onSkipToNext() { MainActivity.sendCommand("next"); }
            @Override public void onSkipToPrevious() { MainActivity.sendCommand("prev"); }
            @Override public void onSeekTo(long pos) { MainActivity.sendCommand("seek:" + (pos / 1000.0)); }
        });
        session.setActive(true);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_TOGGLE.equals(action)) MainActivity.sendCommand("toggle");
        else if (ACTION_NEXT.equals(action)) MainActivity.sendCommand("next");
        else if (ACTION_PREV.equals(action)) MainActivity.sendCommand("prev");
        else if (ACTION_CLOSE.equals(action)) {
            MainActivity.sendCommand("pause");
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        } else if (ACTION_UPDATE.equals(action)) {
            applyUpdate(intent.getStringExtra(EXTRA_JSON));
        }
        showNotification();
        return START_NOT_STICKY;
    }

    private void applyUpdate(String json) {
        long durationMs = 0, positionMs = 0;
        float rate = 1f;
        try {
            JSONObject o = new JSONObject(json);
            title = o.optString("title", "");
            artist = o.optString("artist", "");
            album = o.optString("album", "");
            playing = o.optBoolean("playing");
            durationMs = (long) (o.optDouble("duration", 0) * 1000);
            positionMs = (long) (o.optDouble("position", 0) * 1000);
            rate = (float) o.optDouble("rate", 1);
            if (o.has("art")) {
                String b64 = o.optString("art", "");
                if (b64.isEmpty()) art = null;
                else {
                    byte[] bytes = Base64.decode(b64, Base64.DEFAULT);
                    art = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                }
            }
        } catch (Exception ignored) {
            return;
        }
        MediaMetadata.Builder meta = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, album)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs);
        if (art != null) meta.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art);
        session.setMetadata(meta.build());
        session.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                        | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_STOP)
                .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                        positionMs, playing ? rate : 0f, SystemClock.elapsedRealtime())
                .build());
        if (playing && !noisyRegistered) {
            registerReceiver(noisy, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
            noisyRegistered = true;
        } else if (!playing && noisyRegistered) {
            unregisterReceiver(noisy);
            noisyRegistered = false;
        }
    }

    private PendingIntent servicePending(String action, int code) {
        Intent i = new Intent(this, PlaybackService.class).setAction(action);
        return PendingIntent.getService(this, code, i, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private Notification.Action action(int icon, String label, String act, int code) {
        return new Notification.Action.Builder(Icon.createWithResource(this, icon), label, servicePending(act, code)).build();
    }

    private void showNotification() {
        Intent open = new Intent(this, MainActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        b.setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(title.isEmpty() ? getString(R.string.app_name) : title)
                .setContentText(artist)
                .setContentIntent(content)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setShowWhen(false)
                .setOngoing(playing)
                .setColor(0xFF6B3FE4)
                .addAction(action(R.drawable.ic_prev, "Previous", ACTION_PREV, 1))
                .addAction(action(playing ? R.drawable.ic_pause : R.drawable.ic_play, playing ? "Pause" : "Play", ACTION_TOGGLE, 2))
                .addAction(action(R.drawable.ic_next, "Next", ACTION_NEXT, 3))
                .addAction(action(R.drawable.ic_close, "Close", ACTION_CLOSE, 4))
                .setStyle(new Notification.MediaStyle()
                        .setMediaSession(session.getSessionToken())
                        .setShowActionsInCompactView(0, 1, 2));
        if (art != null) b.setLargeIcon(art);
        Notification n = b.build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        else startForeground(NOTIFICATION_ID, n);
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // App swiped away from recents: stop the music and the notification.
        stopForeground(true);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (noisyRegistered) { unregisterReceiver(noisy); noisyRegistered = false; }
        if (session != null) { session.setActive(false); session.release(); }
        stopForeground(true);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
