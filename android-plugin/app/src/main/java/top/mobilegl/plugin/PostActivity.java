package top.mobilegl.plugin;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.ref.WeakReference;
import java.util.Locale;

public final class PostActivity extends Activity {
    private static final String TAG = "MobileGLPost";

    private static final int COLOR_BACKGROUND = 0xFF121212;
    private static final int COLOR_TEXT = 0xFFEEEEEE;
    private static final int COLOR_PASS = 0xFF4CAF50;
    private static final int COLOR_WARN = 0xFFFF9800;
    private static final int COLOR_FAIL = 0xFFF44336;
    private static final int COLOR_INFO = 0xFF9E9E9E;

    /**
     * Single-flight state: the driver POST runs at most once per process.
     * All fields below are guarded by {@link #POST_LOCK}. Once {@code postCompleted}
     * is set, {@code cachedReportJson}/{@code cachedFailure} never change again.
     */
    private static final Object POST_LOCK = new Object();
    private static boolean postInFlight = false;
    private static boolean postCompleted = false;
    private static String cachedReportJson;
    private static Throwable cachedFailure;
    private static WeakReference<PostActivity> deliveryTarget = new WeakReference<>(null);

    private static boolean nativeLoaded = false;

    private LinearLayout contentLayout;
    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        contentLayout = new LinearLayout(this);
        contentLayout.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(16);
        contentLayout.setPadding(padding, padding, padding, padding);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(COLOR_BACKGROUND);
        scrollView.setFillViewport(true);
        scrollView.addView(contentLayout);
        setContentView(scrollView);

        addText("MobileGL Driver POST", 20, COLOR_TEXT, true, 0);
        statusView = addText("Running self-test...", 14, COLOR_INFO, false, dp(8));

        boolean startWorker = false;
        boolean renderNow = false;
        synchronized (POST_LOCK) {
            deliveryTarget = new WeakReference<>(this);
            if (postCompleted) {
                renderNow = true;
            } else if (!postInFlight) {
                postInFlight = true;
                startWorker = true;
            }
            // else: a probe is already in flight; leave "Running self-test..."
            // showing and wait for delivery to this (the current) activity.
        }
        if (renderNow) {
            renderCachedResult();
            return;
        }
        if (startWorker) {
            new Thread(PostActivity::runDriverPost, "MobileGLDriverPost").start();
        }
    }

    @Override
    protected void onDestroy() {
        synchronized (POST_LOCK) {
            if (deliveryTarget.get() == this) {
                deliveryTarget = new WeakReference<>(null);
            }
        }
        super.onDestroy();
    }

    /**
     * Loads libMobileGL.so on first use, off the UI thread, so the first frame
     * renders immediately. Any failure (including errors thrown by static
     * initializers of the library) is captured and rethrown so it surfaces
     * through the normal error path.
     */
    private static synchronized void ensureNativeLoaded() {
        if (nativeLoaded) {
            return;
        }
        try {
            System.loadLibrary("MobileGL");
        } catch (Throwable error) {
            throw new IllegalStateException("Failed to load libMobileGL.so: " + error, error);
        }
        nativeLoaded = true;
    }

    private static void runDriverPost() {
        String json = null;
        Throwable failure = null;
        try {
            ensureNativeLoaded();
            json = nativeRunDriverPost();
            Log.i(TAG, json == null ? "<null report>" : json);
        } catch (Throwable error) {
            Log.e(TAG, "Driver POST failed", error);
            failure = error;
        }
        synchronized (POST_LOCK) {
            cachedReportJson = json;
            cachedFailure = failure;
            postCompleted = true;
            postInFlight = false;
        }
        deliverResult();
    }

    /** Delivers the cached result to whichever activity instance is current, if any. */
    private static void deliverResult() {
        final PostActivity target;
        synchronized (POST_LOCK) {
            target = deliveryTarget.get();
        }
        if (target == null) {
            return;
        }
        target.runOnUiThread(() -> {
            if (target.isFinishing() || target.isDestroyed()) {
                return;
            }
            target.renderCachedResult();
        });
    }

    private void renderCachedResult() {
        String json;
        Throwable failure;
        synchronized (POST_LOCK) {
            json = cachedReportJson;
            failure = cachedFailure;
        }
        if (failure != null) {
            showError("Driver POST failed: " + failure);
            return;
        }
        renderReport(json);
    }

    private void renderReport(String json) {
        if (json == null || json.isEmpty()) {
            showError("Driver POST returned an empty report.");
            return;
        }
        JSONObject root;
        try {
            root = new JSONObject(json);
        } catch (JSONException error) {
            showError("Failed to parse POST report: " + error.getMessage() + "\n\n" + json);
            return;
        }
        String nativeError = root.optString("error", "");
        if (!nativeError.isEmpty()) {
            showError(nativeError);
            addText(json, 10, COLOR_INFO, false, dp(8));
            return;
        }
        JSONArray backends = extractBackends(root);
        if (backends.length() == 0) {
            showError("POST report contains no backend sections:\n\n" + json);
            return;
        }
        statusView.setText("Self-test complete.");
        for (int i = 0; i < backends.length(); ++i) {
            JSONObject backend = backends.optJSONObject(i);
            if (backend != null) {
                renderBackendSection(backend);
            }
        }
    }

    private static JSONArray extractBackends(JSONObject root) {
        JSONArray backends = root.optJSONArray("backends");
        if (backends != null) {
            return backends;
        }
        backends = new JSONArray();
        String[] keys = {"gles", "DirectGLES", "vulkan", "DirectVulkan"};
        for (String key : keys) {
            JSONObject backend = root.optJSONObject(key);
            if (backend == null) {
                continue;
            }
            if (!backend.has("backend") && !backend.has("name")) {
                try {
                    backend.put("backend", key);
                } catch (JSONException ignored) {
                }
            }
            backends.put(backend);
        }
        return backends;
    }

    private void renderBackendSection(JSONObject backend) {
        String name = backend.optString("backend", backend.optString("name", "Unknown backend"));
        addText(sectionTitle(name), 16, COLOR_TEXT, true, dp(20));

        if (!backend.optBoolean("available", true)) {
            addText("Driver not present / not probeable", 12, COLOR_INFO, false, dp(4));
            return;
        }

        String verdict = backend.optString("verdict", "UNKNOWN");
        addText("Verdict: " + verdict, 14, verdictColor(verdict), true, dp(4));

        String renderer = backend.optString("renderer", "");
        if (!renderer.isEmpty()) {
            addText(renderer, 12, COLOR_INFO, false, dp(2));
        }

        JSONArray checks = backend.optJSONArray("checks");
        if (checks == null) {
            return;
        }
        for (int i = 0; i < checks.length(); ++i) {
            JSONObject check = checks.optJSONObject(i);
            if (check == null) {
                continue;
            }
            String status = check.optString("status", "INFO");
            String checkName = check.optString("name", "unnamed check");
            String detail = check.optString("detail", check.optString("message", ""));
            String row = "[" + status + "] " + checkName + (detail.isEmpty() ? "" : " - " + detail);
            addText(row, 12, statusColor(status), false, dp(4));
        }
    }

    private static String sectionTitle(String backendName) {
        String lower = backendName.toLowerCase(Locale.ROOT);
        if (lower.contains("gles") || lower.contains("espryt")) {
            return "DirectGLES (Espryt)";
        }
        if (lower.contains("vulkan") || lower.contains("magma")) {
            return "DirectVulkan (Magma)";
        }
        return backendName;
    }

    private static int verdictColor(String verdict) {
        switch (verdict.toUpperCase(Locale.ROOT)) {
            case "OK":
                return COLOR_PASS;
            case "DEGRADED":
                return COLOR_WARN;
            case "UNSUPPORTED":
                return COLOR_FAIL;
            default:
                return COLOR_INFO;
        }
    }

    private static int statusColor(String status) {
        switch (status.toUpperCase(Locale.ROOT)) {
            case "PASS":
                return COLOR_PASS;
            case "WARN":
                return COLOR_WARN;
            case "FAIL":
                return COLOR_FAIL;
            case "INFO":
                return COLOR_INFO;
            default:
                return COLOR_TEXT;
        }
    }

    private void showError(String message) {
        Log.e(TAG, message);
        statusView.setText("Self-test failed.");
        statusView.setTextColor(COLOR_FAIL);
        addText(message, 12, COLOR_FAIL, false, dp(8));
    }

    private TextView addText(String text, int sizeSp, int color, boolean bold, int topMarginPx) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTypeface(Typeface.MONOSPACE, bold ? Typeface.BOLD : Typeface.NORMAL);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.topMargin = topMarginPx;
        contentLayout.addView(view, params);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static native String nativeRunDriverPost();
}
