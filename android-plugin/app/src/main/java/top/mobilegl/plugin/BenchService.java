package top.mobilegl.plugin;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * Hosts one driver-benchmark run in its own process ({@code android:process=":bench"})
 * and exits when done.
 *
 * The isolation is load-bearing, not defensive: MobileGL latches its backend from
 * {@code MOBILEGL_BACKEND_TYPE} on first initialization, so Espryt and Magma can never
 * share a process, and Espryt's teardown terminates the process-default EGL display,
 * which would take the POST activity's HWUI context down with it. A fresh process per
 * tap sidesteps both, and {@link System#exit} at the end guarantees the next tap gets
 * one.
 */
public final class BenchService extends Service {
    private static final String TAG = "MobileGLBench";

    public static final String ACTION_RESULT = "top.mobilegl.plugin.BENCH_RESULT";
    public static final String EXTRA_BACKEND = "backend";
    public static final String EXTRA_RESULT_JSON = "resultJson";
    public static final String EXTRA_FRAMES = "frames";
    public static final String EXTRA_WARMUP = "warmupFrames";

    private static native String nativeRunDriverBench(String backendType, int frames, int warmupFrames);

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String backend = intent != null ? intent.getStringExtra(EXTRA_BACKEND) : null;
        final int frames = intent != null ? intent.getIntExtra(EXTRA_FRAMES, 120) : 120;
        final int warmup = intent != null ? intent.getIntExtra(EXTRA_WARMUP, 30) : 30;
        if (backend == null) {
            stopSelf();
            return START_NOT_STICKY;
        }

        new Thread(() -> {
            String result;
            try {
                System.loadLibrary("MobileGL");
                result = nativeRunDriverBench(backend, frames, warmup);
            } catch (Throwable t) {
                Log.e(TAG, "bench failed", t);
                result = "{\"error\":\"" + t.getClass().getSimpleName() + ": "
                        + String.valueOf(t.getMessage()).replace("\\", "\\\\").replace("\"", "\\\"")
                        + "\"}";
            }

            Intent reply = new Intent(ACTION_RESULT);
            reply.setPackage(getPackageName());
            reply.putExtra(EXTRA_BACKEND, backend);
            reply.putExtra(EXTRA_RESULT_JSON, result);
            sendBroadcast(reply);
            Log.i(TAG, "bench done for " + backend + " (" + result.length() + " bytes)");

            // The broadcast is already handed to system_server; give binder a
            // moment, then take the whole process down so the next run starts
            // from an uninitialized MobileGL.
            try {
                Thread.sleep(250);
            } catch (InterruptedException ignored) {
            }
            stopSelf();
            System.exit(0);
        }, "MobileGLDriverBench").start();

        return START_NOT_STICKY;
    }
}
