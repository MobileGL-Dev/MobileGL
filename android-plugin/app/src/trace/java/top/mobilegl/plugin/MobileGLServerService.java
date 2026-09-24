package top.mobilegl.plugin;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.List;
import java.util.Map;

/** Trace-only entry point for a TCP supervisor; EGL belongs to its session children. */
public final class MobileGLServerService extends Service {
    private static final String TAG = "MobileGLServer";
    private static final String CHANNEL = "mobilegl-server";
    // P12 (D8): the on-screen display server's process. Only one server is active on the device.
    private static final long DISPLAY_SERVER_EXIT_WAIT_MS = 5000;
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

    // P12 (D8): ONLY ONE SERVER IS ACTIVE ON THE DEVICE. The on-screen display server runs in the
    // display Activity's process (:mglwin) and listens on the same port; before the offscreen
    // supervisor is exec'd, that Activity's task is removed (so it is not restarted from recents)
    // and its process is killed - same uid, so Process.killProcess may - and waited out, bounded,
    // so its listener is gone before the supervisor binds.
    private void stopDisplayServer() {
        ActivityManager activities = getSystemService(ActivityManager.class);
        ComponentName display = new ComponentName(this, MobileGLDisplayActivity.class);
        try {
            for (ActivityManager.AppTask task : activities.getAppTasks()) {
                ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                if (info != null && display.equals(info.baseIntent.getComponent())) {
                    Log.i(TAG, "removing the on-screen display server's task (P12 D8: one server at a time)");
                    task.finishAndRemoveTask();
                }
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "could not inspect the display server's task", error);
        }
        String processName = getPackageName() + ":mglwin";
        long deadline = SystemClock.uptimeMillis() + DISPLAY_SERVER_EXIT_WAIT_MS;
        int killed = -1;
        for (;;) {
            int pid = -1;
            List<ActivityManager.RunningAppProcessInfo> processes = activities.getRunningAppProcesses();
            if (processes != null) {
                for (ActivityManager.RunningAppProcessInfo process : processes) {
                    if (processName.equals(process.processName)) pid = process.pid;
                }
            }
            // AMS forgets a dead process a moment after it is gone; /proc is the ground truth.
            if (pid < 0 || !new File("/proc/" + pid).exists()) {
                if (killed > 0) Log.i(TAG, "on-screen display server " + processName + " pid=" + killed + " is gone");
                return;
            }
            if (pid != killed) {
                Log.i(TAG, "stopping the on-screen display server " + processName + " pid=" + pid
                        + " (P12 D8: one server at a time)");
                android.os.Process.killProcess(pid);
                killed = pid;
            }
            if (SystemClock.uptimeMillis() >= deadline) {
                Log.e(TAG, "on-screen display server " + processName + " pid=" + pid + " still exists after "
                        + DISPLAY_SERVER_EXIT_WAIT_MS + " ms; starting the supervisor anyway");
                return;
            }
            SystemClock.sleep(50);
        }
    }

    @Override public synchronized int onStartCommand(Intent intent, int flags, int startId) {
        if (supervisor != null && supervisor.isAlive()) {
            Log.w(TAG, "supervisor already running; stop the service before reconfiguring");
            return START_NOT_STICKY;
        }
        stopDisplayServer();
        String endpoint = intent == null ? null : intent.getStringExtra("listen");
        if (endpoint == null) endpoint = "tcp://127.0.0.1:40613";
        try {
            File executable = new File(getApplicationInfo().nativeLibraryDir, "libMobileGLServer.so");
            ProcessBuilder builder = new ProcessBuilder(executable.getAbsolutePath(), endpoint, "--serve");
            builder.directory(getFilesDir()).redirectErrorStream(true);
            Map<String, String> env = builder.environment();
            // The replay runner's KEY=VALUE;KEY grammar (unset and empty values included) and the
            // server role's forced keys, shared with MobileGLDisplayActivity (P12 D7).
            ServerEnvironment.applyServerRole(env,
                    intent == null ? null : intent.getStringExtra("env"),
                    intent == null ? null : intent.getStringExtra("token"),
                    /*backend=*/null,
                    new File(getFilesDir(), "mgl.log").getAbsolutePath());
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
