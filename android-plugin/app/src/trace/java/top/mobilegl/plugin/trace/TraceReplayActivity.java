package top.mobilegl.plugin.trace;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowManager;
import android.widget.TextView;

import java.io.File;

public final class TraceReplayActivity extends Activity {
    public static final String ACTION_TRACE_REPLAY = "top.mobilegl.plugin.TRACE_REPLAY";

    private static final String TAG = "MobileGLTraceRunner";
    static {
        System.loadLibrary("trace_replay_runner");
    }

    private TextView statusView;
    private TraceReplayRequest request;
    private boolean started;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        window.addFlags(
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                        | WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                        | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
        );
        SurfaceView surfaceView = new SurfaceView(this);
        setContentView(surfaceView);

        Intent intent = getIntent();
        request = TraceReplayRequest.from(
                intent,
                getFilesDir(),
                getString(top.mobilegl.plugin.R.string.mobilegl_default_backend)
        );
        statusView = new TextView(this);
        statusView.setText("Waiting for render surface\n" + request.outputDir);
        statusView.setPadding(24, 24, 24, 24);
        addContentView(statusView, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.WRAP_CONTENT
        ));

        SurfaceHolder holder = surfaceView.getHolder();
        if (request.width > 0 && request.height > 0) {
            holder.setFixedSize(request.width, request.height);
        }
        holder.addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                scheduleReplay(holder);
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                scheduleReplay(holder);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
            }
        });
    }

    private void scheduleReplay(SurfaceHolder holder) {
        mainHandler.postDelayed(() -> startReplay(holder), 250);
    }

    private void startReplay(SurfaceHolder holder) {
        if (started) {
            return;
        }
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid()) {
            return;
        }
        started = true;
        statusView.setText("Running trace replay\n" + request.outputDir);
        new Thread(() -> runRequest(request, surface), "MobileGLTraceReplay").start();
    }

    private void runRequest(TraceReplayRequest request, Surface surface) {
        TraceReplayResult result = nativeRunTraceReplay(
                surface,
                request.tracePath,
                request.goldenPath,
                request.outputDir,
                request.backend,
                request.targetFrame,
                request.targetCall,
                request.width,
                request.height,
                request.tolerance,
                request.cropX,
                request.cropY,
                request.cropWidth,
                request.cropHeight,
                request.fuzzPercent
        );
        Log.i(TAG, result.toString());
        TraceReplayResult finalResult = result;
        runOnUiThread(() -> {
            statusView.setText(finalResult.toString());
            finish();
        });
    }

    private static native TraceReplayResult nativeRunTraceReplay(
            Surface surface,
            String tracePath,
            String goldenPath,
            String outputDir,
            String backend,
            int targetFrame,
            long targetCall,
            int width,
            int height,
            int tolerance,
            int cropX,
            int cropY,
            int cropWidth,
            int cropHeight,
            int fuzzPercent
    );

    private static final class TraceReplayRequest {
        final String tracePath;
        final String goldenPath;
        final String outputDir;
        final String backend;
        final int targetFrame;
        final long targetCall;
        final int width;
        final int height;
        final int tolerance;
        final int cropX;
        final int cropY;
        final int cropWidth;
        final int cropHeight;
        final int fuzzPercent;

        private TraceReplayRequest(
                String tracePath,
                String goldenPath,
                String outputDir,
                String backend,
                int targetFrame,
                long targetCall,
                int width,
                int height,
                int tolerance,
                int cropX,
                int cropY,
                int cropWidth,
                int cropHeight,
                int fuzzPercent
        ) {
            this.tracePath = tracePath;
            this.goldenPath = goldenPath;
            this.outputDir = outputDir;
            this.backend = backend;
            this.targetFrame = targetFrame;
            this.targetCall = targetCall;
            this.width = width;
            this.height = height;
            this.tolerance = tolerance;
            this.cropX = cropX;
            this.cropY = cropY;
            this.cropWidth = cropWidth;
            this.cropHeight = cropHeight;
            this.fuzzPercent = fuzzPercent;
        }

        static TraceReplayRequest from(Intent intent, File filesDir, String defaultBackend) {
            String outputDir = readString(intent, "output_dir", new File(filesDir, "trace-replay").getAbsolutePath());
            return new TraceReplayRequest(
                    readString(intent, "trace_path", ""),
                    readString(intent, "golden_path", ""),
                    outputDir,
                    readString(intent, "backend", defaultBackend),
                    intent.getIntExtra("target_frame", -1),
                    intent.getLongExtra("target_call", -1L),
                    intent.getIntExtra("width", 0),
                    intent.getIntExtra("height", 0),
                    intent.getIntExtra("tolerance", 0),
                    intent.getIntExtra("crop_x", 0),
                    intent.getIntExtra("crop_y", 0),
                    intent.getIntExtra("crop_width", 0),
                    intent.getIntExtra("crop_height", 0),
                    intent.getIntExtra("fuzz_percent", 20)
            );
        }

        private static String readString(Intent intent, String key, String fallback) {
            String value = intent.getStringExtra(key);
            return value == null ? fallback : value;
        }
    }

    public static final class TraceReplayResult {
        public final boolean passed;
        public final int statusCode;
        public final String message;
        public final String resultPath;
        public final String actualPath;

        public TraceReplayResult(
                boolean passed,
                int statusCode,
                String message,
                String resultPath,
                String actualPath
        ) {
            this.passed = passed;
            this.statusCode = statusCode;
            this.message = message;
            this.resultPath = resultPath;
            this.actualPath = actualPath;
        }

        @Override
        public String toString() {
            return "TraceReplayResult{" +
                    "passed=" + passed +
                    ", statusCode=" + statusCode +
                    ", message='" + message + '\'' +
                    ", resultPath='" + resultPath + '\'' +
                    ", actualPath='" + actualPath + '\'' +
                    '}';
        }
    }
}
