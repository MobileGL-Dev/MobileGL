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

        // P0 spike A: when asked, exec the packaged server stub out of nativeLibraryDir
        // instead of replaying anything. This mode needs no trace and no render surface.
        String spikeLibrary = spawnSpikeLibrary(intent);
        if (spikeLibrary != null) {
            runSpawnSpike(spikeLibrary);
            return;
        }

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
                request.alternateGoldenPath,
                request.outputDir,
                request.diffPath,
                request.backend,
                request.targetFrame,
                request.targetCall,
                request.width,
                request.height,
                request.ssimThreshold,
                request.cropX,
                request.cropY,
                request.cropWidth,
                request.cropHeight,
                request.angleVariant,
                request.useAngle,
                request.usePbuffer,
                request.avoidAngleLlvmpipeSamplerMipmapMinFilter,
                request.avoidAngleLlvmpipeExplicitLodBias,
                request.coherentAsFlush,
                request.fixIterationRPSubgroupScratch,
                request.deriveNumSubgroups,
                request.iterationRPFixBarrier,
                request.texture2dDumps,
                request.benchmark,
                request.benchmarkTailFrames,
                request.benchmarkFinish,
                request.benchmarkResultPath
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
            String alternateGoldenPath,
            String outputDir,
            String diffPath,
            String backend,
            int targetFrame,
            long targetCall,
            int width,
            int height,
            double ssimThreshold,
            int cropX,
            int cropY,
            int cropWidth,
            int cropHeight,
            String angleVariant,
            boolean useAngle,
            boolean usePbuffer,
            boolean avoidAngleLlvmpipeSamplerMipmapMinFilter,
            boolean avoidAngleLlvmpipeExplicitLodBias,
            boolean coherentAsFlush,
            boolean fixIterationRPSubgroupScratch,
            boolean deriveNumSubgroups,
            boolean iterationRPFixBarrier,
            String texture2dDumps,
            boolean benchmark,
            int benchmarkTailFrames,
            boolean benchmarkFinish,
            String benchmarkResultPath
    );


    // ---------------------------------------------------------------------------
    // P0 spike A: prove an APK can ship a second native executable and exec it.
    //
    // The exec has to happen here, in the application's own process: an `adb shell
    // run-as` invocation runs in a different SELinux domain, so it can succeed while
    // the real app is denied. The child reports the domain it ended up in, and the
    // parent reports the domain it spawned from, so the log line stands on its own.
    // ---------------------------------------------------------------------------
    private static final String EXTRA_SPAWN_SPIKE = "mobilegl_spike_spawn";
    private static final String DEFAULT_SPAWN_SPIKE_LIBRARY = "libMobileGLServer.so";

    private static String spawnSpikeLibrary(Intent intent) {
        if (!intent.hasExtra(EXTRA_SPAWN_SPIKE)) {
            return null;
        }
        // Accepts --ez (boolean, arrives as a null string) and --es with either a truthy
        // marker or the library file name to exec.
        String value = intent.getStringExtra(EXTRA_SPAWN_SPIKE);
        if (value == null || value.isEmpty() || "1".equals(value) || "true".equals(value)) {
            return DEFAULT_SPAWN_SPIKE_LIBRARY;
        }
        return value;
    }

    private void runSpawnSpike(String libraryName) {
        // The surface callbacks fire regardless; this keeps them from starting a replay
        // underneath the spike.
        started = true;
        File outputDir = new File(request.outputDir);
        String serverPath = new File(getApplicationInfo().nativeLibraryDir, libraryName)
                .getAbsolutePath();
        String markerPath = new File(outputDir, "spike-spawn.txt").getAbsolutePath();
        statusView.setText("Running spawn spike\n" + serverPath);
        new Thread(() -> {
            outputDir.mkdirs();
            String message = nativeRunSpawnSpike(serverPath, markerPath);
            Log.i(TAG, message);
            runOnUiThread(() -> {
                statusView.setText(message);
                finish();
            });
        }, "MobileGLSpawnSpike").start();
    }

    private static native String nativeRunSpawnSpike(String serverPath, String markerPath);

    private static final class TraceReplayRequest {
        final String tracePath;
        final String goldenPath;
        final String alternateGoldenPath;
        final String outputDir;
        final String diffPath;
        final String backend;
        final int targetFrame;
        final long targetCall;
        final int width;
        final int height;
        final double ssimThreshold;
        final int cropX;
        final int cropY;
        final int cropWidth;
        final int cropHeight;
        final String angleVariant;
        final boolean useAngle;
        final boolean usePbuffer;
        final boolean avoidAngleLlvmpipeSamplerMipmapMinFilter;
        final boolean avoidAngleLlvmpipeExplicitLodBias;
        final boolean coherentAsFlush;
        final boolean fixIterationRPSubgroupScratch;
        final boolean deriveNumSubgroups;
        final boolean iterationRPFixBarrier;
        final String texture2dDumps;
        // Benchmark (frame-timing) mode. Replays the whole trace, times every frame
        // boundary, and skips the snapshot and the SSIM comparison; "passed" then only
        // means the replay ran to the end without error.
        final boolean benchmark;
        final int benchmarkTailFrames;
        final boolean benchmarkFinish;
        final String benchmarkResultPath;

        private TraceReplayRequest(
                String tracePath,
                String goldenPath,
                String alternateGoldenPath,
                String outputDir,
                String diffPath,
                String backend,
                int targetFrame,
                long targetCall,
                int width,
                int height,
                double ssimThreshold,
                int cropX,
                int cropY,
                int cropWidth,
                int cropHeight,
                String angleVariant,
                boolean useAngle,
                boolean usePbuffer,
                boolean avoidAngleLlvmpipeSamplerMipmapMinFilter,
                boolean avoidAngleLlvmpipeExplicitLodBias,
                boolean coherentAsFlush,
                boolean fixIterationRPSubgroupScratch,
                boolean deriveNumSubgroups,
                boolean iterationRPFixBarrier,
                    String texture2dDumps,
                boolean benchmark,
                int benchmarkTailFrames,
                boolean benchmarkFinish,
                String benchmarkResultPath
        ) {
            this.tracePath = tracePath;
            this.goldenPath = goldenPath;
            this.alternateGoldenPath = alternateGoldenPath;
            this.outputDir = outputDir;
            this.diffPath = diffPath;
            this.backend = backend;
            this.targetFrame = targetFrame;
            this.targetCall = targetCall;
            this.width = width;
            this.height = height;
            this.ssimThreshold = ssimThreshold;
            this.cropX = cropX;
            this.cropY = cropY;
            this.cropWidth = cropWidth;
            this.cropHeight = cropHeight;
            this.angleVariant = angleVariant;
            this.useAngle = useAngle;
            this.usePbuffer = usePbuffer;
            this.avoidAngleLlvmpipeSamplerMipmapMinFilter = avoidAngleLlvmpipeSamplerMipmapMinFilter;
            this.avoidAngleLlvmpipeExplicitLodBias = avoidAngleLlvmpipeExplicitLodBias;
            this.coherentAsFlush = coherentAsFlush;
            this.fixIterationRPSubgroupScratch = fixIterationRPSubgroupScratch;
            this.deriveNumSubgroups = deriveNumSubgroups;
            this.iterationRPFixBarrier = iterationRPFixBarrier;
            this.texture2dDumps = texture2dDumps;
            this.benchmark = benchmark;
            this.benchmarkTailFrames = benchmarkTailFrames;
            this.benchmarkFinish = benchmarkFinish;
            this.benchmarkResultPath = benchmarkResultPath;
        }

        static TraceReplayRequest from(Intent intent, File filesDir, String defaultBackend) {
            String outputDir = readString(intent, "output_dir", new File(filesDir, "trace-replay").getAbsolutePath());
            String diffPath = readString(intent, "diff_path", "");
            String benchmarkResultPath =
                    readString(intent, "benchmark_result_path", outputDir + "/benchmark.json");
            return new TraceReplayRequest(
                    readString(intent, "trace_path", ""),
                    readString(intent, "golden_path", ""),
                    readString(intent, "alternate_golden_path", ""),
                    outputDir,
                    diffPath,
                    readString(intent, "backend", defaultBackend),
                    intent.getIntExtra("target_frame", -1),
                    intent.getLongExtra("target_call", -1L),
                    intent.getIntExtra("width", 0),
                    intent.getIntExtra("height", 0),
                    readDouble(intent, "ssim_threshold", 0.99),
                    intent.getIntExtra("crop_x", 0),
                    intent.getIntExtra("crop_y", 0),
                    intent.getIntExtra("crop_width", 0),
                    intent.getIntExtra("crop_height", 0),
                    readString(intent, "angle_variant", ""),
                    intent.getBooleanExtra("use_angle", false),
                    intent.getBooleanExtra("use_pbuffer", false),
                    intent.getBooleanExtra("avoid_angle_llvmpipe_sampler_mipmap_min_filter", false),
                    intent.getBooleanExtra("avoid_angle_llvmpipe_explicit_lod_bias", false),
                    intent.getBooleanExtra("coherent_as_flush", false),
                    intent.getBooleanExtra("fix_iterationrp_subgroup_scratch", false),
                    intent.getBooleanExtra("derive_num_subgroups", false),
                    intent.getBooleanExtra("iterationrp_fix_barrier", false),
                    readString(intent, "texture_2d_dumps", ""),
                    intent.getBooleanExtra("benchmark", false),
                    intent.getIntExtra("benchmark_tail_frames", 200),
                    intent.getBooleanExtra("benchmark_finish", true),
                    benchmarkResultPath
            );
        }

        private static String readString(Intent intent, String key, String fallback) {
            String value = intent.getStringExtra(key);
            return value == null ? fallback : value;
        }

        private static double readDouble(Intent intent, String key, double fallback) {
            String stringValue = intent.getStringExtra(key);
            if (stringValue != null && !stringValue.isEmpty()) {
                try {
                    return Double.parseDouble(stringValue);
                } catch (NumberFormatException ignored) {
                    return fallback;
                }
            }
            return intent.getDoubleExtra(key, fallback);
        }
    }

    public static final class TraceReplayResult {
        public final boolean passed;
        public final int statusCode;
        public final String message;
        public final String resultPath;
        public final String actualPath;
        public final String diffPath;

        public TraceReplayResult(
                boolean passed,
                int statusCode,
                String message,
                String resultPath,
                String actualPath,
                String diffPath
        ) {
            this.passed = passed;
            this.statusCode = statusCode;
            this.message = message;
            this.resultPath = resultPath;
            this.actualPath = actualPath;
            this.diffPath = diffPath;
        }

        @Override
        public String toString() {
            return "TraceReplayResult{" +
                    "passed=" + passed +
                    ", statusCode=" + statusCode +
                    ", message='" + message + '\'' +
                    ", resultPath='" + resultPath + '\'' +
                    ", actualPath='" + actualPath + '\'' +
                    ", diffPath='" + diffPath + '\'' +
                    '}';
        }
    }
}
