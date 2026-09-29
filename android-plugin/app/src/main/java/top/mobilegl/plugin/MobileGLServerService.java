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

/** Entry point for the offscreen TCP supervisor; EGL belongs to its session children. */
public final class MobileGLServerService extends Service {
    private static final String TAG = "MobileGLServer";
    private static final String CHANNEL = "mobilegl-server";
    // P12 (D8): the on-screen display server's process. Only one server is active on the device.
    private static final long DISPLAY_SERVER_EXIT_WAIT_MS = 5000;
    private volatile Process supervisor;
    // P11 B1: the private abstract listener beside a TCP-only supervisor (see BrokerTarget).
    private volatile Process brokerSupervisor;
    private PowerManager.WakeLock wakeLock;

    /**
     * P11 B1 (CONTRACT-P11 B1): where {@link ExternalClientBroker} connects for a same-device client in
     * another security context, and the token it checks. A unix endpoint (@name or a path) is used as
     * it is; a TCP-only server gets a second supervisor on a private abstract name beside it - same
     * binary, same environment and token - because the broker's descriptors must be a unix pair to
     * carry the shared segments. Only this app's processes can connect to either (SELinux).
     */
    static final class BrokerTarget {
        final String endpoint;
        final String token;
        final Process process;
        // The core the server's `auto` policy gives a dialled-in shared-segment client's apply thread
        // (ServerEnvironment.reservedApplyCore); the broker hands it to the helper, which keeps the
        // client off it. 0 = none reserved.
        final long applyCore;

        BrokerTarget(String endpoint, String token, Process process, long applyCore) {
            this.endpoint = endpoint;
            this.token = token;
            this.process = process;
            this.applyCore = applyCore;
        }
    }

    /** cpuinfo_max_freq of every cpu this process can count (0 where unreadable). */
    private static long[] cpuMaxFrequenciesKHz() {
        int count = Math.min(64, Math.max(1, Runtime.getRuntime().availableProcessors()));
        long[] frequencies = new long[count];
        for (int cpu = 0; cpu < count; cpu++) {
            File file = new File("/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/cpuinfo_max_freq");
            try (BufferedReader reader = new BufferedReader(new java.io.FileReader(file))) {
                frequencies[cpu] = Long.parseLong(reader.readLine().trim());
            } catch (Exception unreadable) {
                frequencies[cpu] = 0;
            }
        }
        return frequencies;
    }

    // Set when a supervisor the broker can reach is running, cleared when it stops. The broker runs
    // in this same process (:mglsrv), so a broadcast that finds null here finds no server.
    private static volatile BrokerTarget brokerTarget;

    /** The running server's broker target, or null when none is running. */
    static BrokerTarget brokerTarget() {
        BrokerTarget target = brokerTarget;
        return target != null && target.process != null && target.process.isAlive() ? target : null;
    }

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
        final String envExtra = intent == null ? null : intent.getStringExtra("env");
        final String token = intent == null ? null : intent.getStringExtra("token");
        java.util.Map<String, String> requested = new java.util.HashMap<>();
        ServerEnvironment.apply(requested, envExtra);
        final long applyCore = ServerEnvironment.reservedApplyCore(cpuMaxFrequenciesKHz(),
                requested.get("MOBILEGL_IPC_SERVER_AFFINITY"));
        try {
            supervisor = startSupervisor(endpoint, envExtra, token, "mgl-supervisor-log", startId, true);
            // P11 B1: the broker's target. A unix endpoint is reachable by this app as it is (a
            // relative path resolves against filesDir, the supervisor's working directory); a TCP
            // one gets a private abstract listener of its own (BrokerTarget).
            if (endpoint.startsWith("tcp://")) {
                String privateName = "@" + getPackageName() + ".broker." + Long.toHexString(
                        new java.security.SecureRandom().nextLong() & 0xffffffffL);
                brokerSupervisor = startSupervisor(privateName, envExtra, token, "mgl-broker-supervisor-log",
                        startId, false);
                brokerTarget = new BrokerTarget(privateName, token, brokerSupervisor, applyCore);
                Log.i(TAG, "same-device external clients: broker listener " + privateName
                        + " beside the TCP-only supervisor on " + endpoint + "; apply core reserved for them 0x"
                        + Long.toHexString(applyCore));
            } else {
                String unix = endpoint.startsWith("@") || endpoint.startsWith("/") ? endpoint
                        : new File(getFilesDir(), endpoint).getAbsolutePath();
                brokerTarget = new BrokerTarget(unix, token, supervisor, applyCore);
                Log.i(TAG, "same-device external clients: the broker connects to " + unix
                        + "; apply core reserved for them 0x" + Long.toHexString(applyCore));
            }
        } catch (Exception error) {
            Log.e(TAG, "cannot start TCP supervisor", error);
            brokerTarget = null;
            stopSelf(startId);
        }
        return START_NOT_STICKY;
    }

    /**
     * Execs one {@code libMobileGLServer.so <endpoint> --serve} with the server role's environment and
     * logs its output. When {@code primary}, its exit stops the service.
     */
    private Process startSupervisor(String endpoint, String envExtra, String token, String logThreadName,
                                    int startId, boolean primary) throws java.io.IOException {
        File executable = new File(getApplicationInfo().nativeLibraryDir, "libMobileGLServer.so");
        ProcessBuilder builder = new ProcessBuilder(executable.getAbsolutePath(), endpoint, "--serve");
        builder.directory(getFilesDir()).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        // The replay runner's KEY=VALUE;KEY grammar (unset and empty values included) and the
        // server role's forced keys, shared with MobileGLDisplayActivity (P12 D7).
        ServerEnvironment.applyServerRole(env, envExtra, token, /*backend=*/null,
                new File(getFilesDir(), primary ? "mgl.log" : "mgl.broker.log").getAbsolutePath());
        env.put("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir);
        final Process child = builder.start();
        new Thread(() -> {
            try (BufferedReader output = new BufferedReader(new InputStreamReader(child.getInputStream()))) {
                String line;
                while ((line = output.readLine()) != null) Log.i(TAG, line);
                Log.i(TAG, (primary ? "supervisor" : "broker supervisor") + " exited " + child.waitFor());
            } catch (java.io.InterruptedIOException stopped) {
                // onDestroy's Process.destroy() closes this stream under the read: the service
                // is being stopped (P12 D8: the display Activity stops it), not failing.
                Log.i(TAG, "supervisor output closed: the service is stopping");
            } catch (Exception error) {
                Log.e(TAG, "supervisor output failed", error);
            } finally {
                if (primary) stopSelf(startId);
            }
        }, logThreadName).start();
        return child;
    }

    @Override public void onDestroy() {
        brokerTarget = null;
        Process child = supervisor;
        if (child != null) child.destroy();
        Process broker = brokerSupervisor;
        if (broker != null) broker.destroy();
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }
}
