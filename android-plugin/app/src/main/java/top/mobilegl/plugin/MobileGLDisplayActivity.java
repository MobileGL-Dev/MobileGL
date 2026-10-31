package top.mobilegl.plugin;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/**
 * P12 (on-screen server window), D7: the ON-SCREEN display server.
 *
 * <p>A headless client (MOBILEGL_IPC_SURFACE=server) asks the server to create its window surface
 * on the SERVER's window (WindowKind::ServerOwned) and renders as if it were drawing straight on
 * this phone's GPU and screen. That window is this Activity's SurfaceView, and because an
 * ANativeWindow survives neither exec nor fork, the TCP server runs IN THIS PROCESS
 * (android:process=":mglwin") on a thread - libMobileGL's mobilegl_server_serve_inprocess, the
 * exec'd supervisor's `--serve` with threads instead of session children. Sessions run one at a
 * time; each one's window surface goes onto the SurfaceView.
 *
 * <p>ONE SERVER AT A TIME (D8). Starting this Activity stops {@link MobileGLServerService} (the
 * offscreen supervisor, whose session children die with it); starting that service kills this
 * process. Whichever was started last owns the port.
 *
 * <p>Extras (the service's names): {@code listen} (tcp://host:port, default loopback 40613),
 * {@code token}, {@code env} (the service's {@code KEY=VALUE;KEY} grammar, {@link ServerEnvironment}),
 * and optionally {@code backend} (MOBILEGL_BACKEND_TYPE: the pin for this process; without it the
 * first session's backend is pinned). The environment is applied to this process with
 * Os.setenv / Os.unsetenv BEFORE libMobileGL is loaded - it reads its knobs at load and first use.
 *
 * <p>THE SURFACE. surfaceCreated / surfaceChanged hand the window to the native display (which takes
 * its own reference); surfaceDestroyed BLOCKS, bounded (~3 s), until a session rendering into it has
 * released its backend surface on the server's apply thread - the GLSurfaceView contract: nothing
 * renders into the window after this callback returns. That session is then latched by name
 * (ServerWindowLost) and its client reads a clean device loss; the server keeps listening and the
 * next session renders once the window is back. A size the client asks for arrives as a geometry
 * request: SurfaceHolder.setFixedSize(w, h) with the view aspect-fitted (letterboxed) into the
 * screen, or setSizeFromLayout() for 0/0.
 *
 * <p>ONE SERVER PER PROCESS LAUNCH. Finishing the Activity stops the server and ends the process, so
 * the next launch starts with a fresh environment, a fresh backend pin and no EGL state.
 * hardwareAccelerated="false" keeps HWUI off EGL in this process (Espryt's teardown terminates the
 * process-default EGL display).
 */
public final class MobileGLDisplayActivity extends Activity {
    private static final String TAG = "MobileGLDisplay";
    static final String DEFAULT_LISTEN = "tcp://127.0.0.1:40613";

    // ---- process-wide: one in-process display server per :mglwin process ------------------------
    private static final Object serverLock = new Object();
    private static Thread serverThread;        // guarded by serverLock
    private static String serverConfiguration; // guarded by serverLock
    private static boolean serverReturned;     // guarded by serverLock: nativeServe came back
    private static volatile boolean displayInstalled;
    private static final Handler mainHandler = new Handler(Looper.getMainLooper());
    // Main thread only.
    private static MobileGLDisplayActivity current;
    private static int requestedWidth;  // the last geometry request; 0/0 = the layout's size
    private static int requestedHeight;

    private FrameLayout container;
    private SurfaceView surfaceView;
    private TextView statusView;

    // ---- native (MobileGL/MG_Remote/Server/DisplayServerJni.cpp) ---------------------------------
    private static native void nativeInstallDisplay();
    private static native int nativeServe(String listen);
    private static native void nativeStop();
    private static native boolean nativeAttachWindow(Surface surface, int width, int height);
    private static native String nativeDetachWindow();
    private static native void nativeUninstallDisplay();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        container = new FrameLayout(this);
        container.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        container.addView(surfaceView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setPadding(24, 24, 24, 24);
        statusView.setVisibility(View.GONE);
        container.addView(statusView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP));
        setContentView(container);
        hideSystemBars();
        // Aspect-fit follows the screen (rotation keeps this Activity: configChanges).
        container.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
                mainHandler.post(this::applyLayout));
        current = this;

        // D8: ONE SERVER AT A TIME. The offscreen supervisor goes first; the in-process listen
        // retries for ~5 s while its listener dies.
        if (stopService(new Intent(this, MobileGLServerService.class))) {
            Log.i(TAG, "stopped MobileGLServerService (the offscreen supervisor): one server at a time (P12 D8)");
        }
        if (!startServer(getIntent())) return;

        SurfaceHolder holder = surfaceView.getHolder();
        // A recreated Activity keeps the size the live session asked for.
        if (requestedWidth > 0 && requestedHeight > 0) holder.setFixedSize(requestedWidth, requestedHeight);
        holder.addCallback(surfaceCallback);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        startServer(intent); // the server is up: this only says whether the configuration differs
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (current == this) current = null;
        if (isFinishing()) {
            boolean serving;
            synchronized (serverLock) {
                serving = serverThread != null && !serverReturned;
            }
            if (serving) {
                // Non-blocking: the live session ends within a control slice, serve() returns and
                // ends this process (onServerReturned).
                Log.i(TAG, "the display Activity is finishing: stopping the in-process display server");
                nativeStop();
            } else {
                endProcess();
            }
        }
        super.onDestroy();
    }

    // ---- the server ----------------------------------------------------------------------------

    private static String extra(Intent intent, String name, String fallback) {
        String value = intent == null ? null : intent.getStringExtra(name);
        return value == null ? fallback : value;
    }

    /** Starts this process's display server once. False (and the Activity finishes) on failure. */
    private boolean startServer(Intent intent) {
        final String listen = extra(intent, "listen", DEFAULT_LISTEN);
        String token = extra(intent, "token", "");
        String env = extra(intent, "env", "");
        String backend = extra(intent, "backend", "");
        String configuration = listen + "\n" + token + "\n" + env + "\n" + backend;
        synchronized (serverLock) {
            if (serverThread != null) {
                if (!configuration.equals(serverConfiguration)) {
                    Log.w(TAG, "the in-process display server already runs in this process with another "
                            + "configuration and keeps it; stop the package (am force-stop) to reconfigure");
                }
                return true;
            }
            // THE ENVIRONMENT BEFORE THE LIBRARY: libMobileGL reads its knobs when it loads and at
            // first use, and the log role is cached at its first line.
            Map<String, String> before = new HashMap<>(System.getenv());
            Map<String, String> after = new HashMap<>(before);
            ServerEnvironment.applyServerRole(after, env, token, backend,
                    new File(getFilesDir(), "mglwin.log").getAbsolutePath());
            try {
                for (Map.Entry<String, String> edit : ServerEnvironment.changes(before, after).entrySet()) {
                    if (edit.getValue() == null) Os.unsetenv(edit.getKey());
                    else Os.setenv(edit.getKey(), edit.getValue(), true);
                }
            } catch (ErrnoException error) {
                fail("could not set the server environment: " + error);
                return false;
            }
            try {
                System.loadLibrary("MobileGL");
                nativeInstallDisplay();
            } catch (Throwable error) {
                // UnsatisfiedLinkError: an APK whose libMobileGL is not a split build has no server.
                fail("libMobileGL has no in-process display server: " + error);
                return false;
            }
            displayInstalled = true;
            serverConfiguration = configuration;
            Log.i(TAG, "starting the in-process display server on " + listen + " (pid=" + Process.myPid()
                    + (backend.isEmpty() ? ", backend pinned by the first session" : ", backend " + backend) + ")");
            serverThread = new Thread(() -> serve(listen), "mgl-display-server");
            serverThread.start();
            return true;
        }
    }

    private static void serve(String listen) {
        int result = nativeServe(listen);
        Log.i(TAG, "in-process display server on " + listen + " returned " + result
                + (result == 0 ? " (stopped)" : " (failed: see the MobileGL lines before this one)"));
        displayInstalled = false;
        nativeUninstallDisplay();
        synchronized (serverLock) {
            serverReturned = true;
        }
        mainHandler.post(() -> onServerReturned(result));
    }

    private static void onServerReturned(int result) {
        MobileGLDisplayActivity activity = current;
        if (activity != null && !activity.isFinishing()) {
            // A display without a server shows nothing anyone can reach: say so and go.
            activity.showStatus("MobileGL display server exited " + result);
            activity.finish(); // onDestroy ends the process
            return;
        }
        endProcess();
    }

    private static void endProcess() {
        Log.i(TAG, "the display server's process ends (one server per :mglwin launch)");
        Process.killProcess(Process.myPid());
    }

    private void fail(String message) {
        Log.e(TAG, message);
        showStatus(message);
        finish();
    }

    private void showStatus(String message) {
        statusView.setText(message);
        statusView.setVisibility(View.VISIBLE);
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    // ---- the window ----------------------------------------------------------------------------

    private final SurfaceHolder.Callback surfaceCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            Rect frame = holder.getSurfaceFrame();
            attach(holder, frame.width(), frame.height(), "surfaceCreated");
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            attach(holder, width, height, "surfaceChanged");
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            if (!displayInstalled) return;
            long started = SystemClock.uptimeMillis();
            // BLOCKS (bounded) until the session rendering into this window has let it go.
            String detached = nativeDetachWindow();
            Log.i(TAG, "surfaceDestroyed: server window detached (" + detached + ") in "
                    + (SystemClock.uptimeMillis() - started) + " ms");
        }
    };

    private void attach(SurfaceHolder holder, int width, int height, String why) {
        if (!displayInstalled) return;
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid()) return;
        boolean attached = nativeAttachWindow(surface, width, height);
        Log.i(TAG, why + ": server window " + width + "x" + height + (attached ? " attached" : " NOT attached"));
    }

    // Called from native (DisplayServerJni.cpp) on the server's APPLY thread while a ServerOwned
    // window surface is created: post to the UI thread and return - never block there.
    @SuppressWarnings("unused")
    private static void onGeometryRequest(int width, int height) {
        mainHandler.post(() -> {
            requestedWidth = Math.max(0, width);
            requestedHeight = Math.max(0, height);
            Log.i(TAG, "geometry request " + requestedWidth + "x" + requestedHeight
                    + (requestedWidth > 0 && requestedHeight > 0 ? " (setFixedSize, aspect-fit)" : " (setSizeFromLayout)"));
            MobileGLDisplayActivity activity = current;
            if (activity != null) activity.applyGeometry();
        });
    }

    private void applyGeometry() {
        SurfaceHolder holder = surfaceView.getHolder();
        if (requestedWidth > 0 && requestedHeight > 0) holder.setFixedSize(requestedWidth, requestedHeight);
        else holder.setSizeFromLayout();
        applyLayout();
    }

    private void applyLayout() {
        int[] fit = ServerEnvironment.aspectFit(container.getWidth(), container.getHeight(),
                requestedWidth, requestedHeight);
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        int width = fit[0] > 0 ? fit[0] : ViewGroup.LayoutParams.MATCH_PARENT;
        int height = fit[1] > 0 ? fit[1] : ViewGroup.LayoutParams.MATCH_PARENT;
        if (params.width != width || params.height != height || params.gravity != Gravity.CENTER) {
            params.width = width;
            params.height = height;
            params.gravity = Gravity.CENTER;
            surfaceView.setLayoutParams(params);
        }
    }
}
