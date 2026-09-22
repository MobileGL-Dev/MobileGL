package top.mobilegl.plugin;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.Map;

/** Trace-only entry point for a TCP supervisor; EGL belongs to its session children. */
public final class MobileGLServerService extends Service {
    private static final String TAG = "MobileGLServer";
    private static final String CHANNEL = "mobilegl-server";
    private volatile Process supervisor;
    private PowerManager.WakeLock wakeLock;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(
                CHANNEL, "MobileGL TCP server", NotificationManager.IMPORTANCE_LOW));
        startForeground(40613, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("MobileGL TCP server")
                .setContentText("Serving remote rendering sessions")
                .setOngoing(true).build());
        wakeLock = getSystemService(PowerManager.class).newWakeLock(
                PowerManager.PARTIAL_WAKE_LOCK, "MobileGL:TCPServer");
        wakeLock.acquire();
    }

    // Keep the replay runner's KEY=VALUE;KEY grammar, including unset and empty values.
    static void applyEnvironment(Map<String, String> env, String overrides) {
        if (overrides == null) return;
        for (String entry : overrides.split(";", -1)) {
            int separator = entry.indexOf('=');
            if (entry.isEmpty() || separator == 0) continue;
            if (separator < 0) env.remove(entry);
            else env.put(entry.substring(0, separator), entry.substring(separator + 1));
        }
    }

    @Override public synchronized int onStartCommand(Intent intent, int flags, int startId) {
        if (supervisor != null && supervisor.isAlive()) {
            Log.w(TAG, "supervisor already running; stop the service before reconfiguring");
            return START_NOT_STICKY;
        }
        String endpoint = intent == null ? null : intent.getStringExtra("listen");
        if (endpoint == null) endpoint = "tcp://127.0.0.1:40613";
        try {
            File executable = new File(getApplicationInfo().nativeLibraryDir, "libMobileGLServer.so");
            ProcessBuilder builder = new ProcessBuilder(executable.getAbsolutePath(), endpoint, "--serve");
            builder.directory(getFilesDir()).redirectErrorStream(true);
            Map<String, String> env = builder.environment();
            applyEnvironment(env, intent == null ? null : intent.getStringExtra("env"));
            env.put("MOBILEGL_IPC_ROLE", "server");
            env.put("MOBILEGL_IPC_DIAL", "no");
            env.put("MOBILEGL_IPC_LOG_FORWARD", "1");
            env.remove("MOBILEGL_IPC_CONTROL");
            env.put("MOBILEGL_IPC_TOKEN", intent == null || intent.getStringExtra("token") == null
                    ? "" : intent.getStringExtra("token"));
            env.putIfAbsent("MOBILEGL_LOG_FILE_PATH", new File(getFilesDir(), "mgl.log").getAbsolutePath());
            env.put("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir);
            supervisor = builder.start();
            final Process child = supervisor;
            new Thread(() -> {
                try (BufferedReader output = new BufferedReader(new InputStreamReader(child.getInputStream()))) {
                    String line;
                    while ((line = output.readLine()) != null) Log.i(TAG, line);
                    Log.i(TAG, "supervisor exited " + child.waitFor());
                } catch (Exception error) {
                    Log.e(TAG, "supervisor output failed", error);
                } finally {
                    stopSelf(startId);
                }
            }, "mgl-supervisor-log").start();
        } catch (Exception error) {
            Log.e(TAG, "cannot start TCP supervisor", error);
            stopSelf(startId);
        }
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        Process child = supervisor;
        if (child != null) child.destroy();
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }
}
