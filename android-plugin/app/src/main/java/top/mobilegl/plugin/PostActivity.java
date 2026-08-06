package top.mobilegl.plugin;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

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
    private static final int COLOR_DETAIL = 0xFFBBBBBB;
    private static final int COLOR_ROW_EVEN = 0xFF1A1A1A;
    private static final int COLOR_ROW_ODD = 0xFF121212;
    private static final int COLOR_TABLE_HEADER = 0xFF303030;
    private static final int COLOR_FORMAT_CELL = 0xFF242424;
    private static final int COLOR_CAPABILITY_FULL = 0xFF2E7D32;
    private static final int COLOR_CAPABILITY_CAVEAT = 0xFFFBC02D;
    private static final int COLOR_CAPABILITY_NONE = 0xFFC62828;

    private static final int FORMAT_COLUMN_WIDTH_DP = 152;
    private static final int CAPABILITY_COLUMN_WIDTH_DP = 152;
    private static final int CAPABILITY_ROW_HEIGHT_DP = 36;

    private static final String INDICATOR_COLLAPSED = "▸";
    private static final String INDICATOR_EXPANDED = "▾";

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

    /**
     * Per-backend bench UI state. Unlike the POST itself (single-flight, latched),
     * a bench may be re-run freely: every tap starts a fresh {@link BenchService}
     * process, so the only state to manage here is the button and the results
     * container the reply renders into.
     */
    private final Map<String, Button> benchButtons = new HashMap<>();
    private final Map<String, LinearLayout> benchResultContainers = new HashMap<>();
    private BroadcastReceiver benchReceiver;

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
        if (benchReceiver != null) {
            unregisterReceiver(benchReceiver);
            benchReceiver = null;
        }
        super.onDestroy();
    }

    /** Registered lazily on the first Run Bench tap; delivers results to the UI thread. */
    private void ensureBenchReceiver() {
        if (benchReceiver != null) {
            return;
        }
        benchReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String backend = intent.getStringExtra(BenchService.EXTRA_BACKEND);
                String json = intent.getStringExtra(BenchService.EXTRA_RESULT_JSON);
                if (backend != null && json != null) {
                    renderBenchResult(backend, json);
                }
            }
        };
        IntentFilter filter = new IntentFilter(BenchService.ACTION_RESULT);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(benchReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(benchReceiver, filter);
        }
    }

    /** Kicks one bench run for a backend in its own service process. */
    private void startBench(String backendType) {
        ensureBenchReceiver();
        Button button = benchButtons.get(backendType);
        if (button != null) {
            button.setEnabled(false);
            button.setText("Bench running... (can take a minute)");
        }
        LinearLayout container = benchResultContainers.get(backendType);
        if (container != null) {
            container.removeAllViews();
        }
        Intent intent = new Intent(this, BenchService.class);
        intent.putExtra(BenchService.EXTRA_BACKEND, backendType);
        intent.putExtra(BenchService.EXTRA_FRAMES, 120);
        intent.putExtra(BenchService.EXTRA_WARMUP, 30);
        startService(intent);
    }

    private void renderBenchResult(String backendType, String json) {
        Button button = benchButtons.get(backendType);
        if (button != null) {
            button.setEnabled(true);
            button.setText("Run Bench");
        }
        LinearLayout container = benchResultContainers.get(backendType);
        if (container == null) {
            return;
        }
        container.removeAllViews();
        try {
            JSONObject root = new JSONObject(json);
            String error = root.optString("error", "");
            if (!error.isEmpty()) {
                container.addView(makeText("Bench failed: " + error, 12, COLOR_FAIL, false));
                return;
            }
            container.addView(makeText(root.optString("renderer", ""), 11, COLOR_INFO, false));
            String header = String.format(Locale.ROOT, "%-22s %10s %10s %9s",
                    "case", "ns/op", "frame ms", "fps");
            container.addView(makeText(header, 11, COLOR_DETAIL, true));
            JSONArray cases = root.optJSONArray("cases");
            if (cases == null) {
                return;
            }
            for (int i = 0; i < cases.length(); ++i) {
                JSONObject row = cases.optJSONObject(i);
                if (row == null) {
                    continue;
                }
                String line = String.format(Locale.ROOT, "%-22s %10.1f %10.3f %9.1f",
                        row.optString("case", "?"),
                        row.optDouble("nsPerOp", 0),
                        row.optDouble("medianFrameMs", 0),
                        row.optDouble("fps", 0));
                TextView view = makeText(line, 11,
                        row.optInt("glError", 0) != 0 ? COLOR_WARN : COLOR_TEXT, false);
                container.addView(view);
            }
        } catch (JSONException error) {
            container.addView(makeText("Bench result unparsable: " + error, 12, COLOR_FAIL, false));
        }
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
            addRawJsonSection(json);
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
        addRawJsonSection(json);
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

        addBenchControls(name);

        JSONArray checks = backend.optJSONArray("checks");
        if (checks != null) {
            LinearLayout table = new LinearLayout(this);
            table.setOrientation(LinearLayout.VERTICAL);
            LinearLayout.LayoutParams tableParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
            );
            tableParams.topMargin = dp(6);
            contentLayout.addView(table, tableParams);
            int rowIndex = 0;
            for (int i = 0; i < checks.length(); ++i) {
                JSONObject check = checks.optJSONObject(i);
                if (check == null) {
                    continue;
                }
                addCheckRow(table, check, rowIndex++);
            }
        }

        renderFormatCapabilities(backend.optJSONObject("formatCapabilities"));
    }

    /** The MOBILEGL_BACKEND_TYPE value a POST section name stands for, or null. */
    private static String backendTypeForSection(String sectionName) {
        switch (sectionName.toLowerCase(Locale.ROOT)) {
            case "gles":
            case "directgles":
                return "DirectGLES";
            case "vulkan":
            case "directvulkan":
                return "DirectVulkan";
            default:
                return null;
        }
    }

    /**
     * One Run Bench button plus the container its results render into. The bench
     * runs the Minecraft-shaped MG_Benchmark cases through the full MobileGL stack
     * on this backend, in a throwaway service process (see BenchService).
     */
    private void addBenchControls(String sectionName) {
        final String backendType = backendTypeForSection(sectionName);
        if (backendType == null) {
            return;
        }
        Button button = new Button(this);
        button.setText("Run Bench");
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        button.setOnClickListener(v -> startBench(backendType));
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        buttonParams.topMargin = dp(6);
        contentLayout.addView(button, buttonParams);
        benchButtons.put(backendType, button);

        LinearLayout results = new LinearLayout(this);
        results.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams resultParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        resultParams.topMargin = dp(4);
        contentLayout.addView(results, resultParams);
        benchResultContainers.put(backendType, results);
    }

    /**
     * Adds one two-column check row (name | status chip) to the table. Rows with
     * a non-empty detail get a collapse indicator and toggle the detail text on tap;
     * rows without detail show no indicator and are not tappable.
     */
    private void addCheckRow(LinearLayout table, JSONObject check, int rowIndex) {
        String status = check.optString("status", "INFO");
        String checkName = check.optString("name", "unnamed check");
        String detail = check.optString("detail", check.optString("message", ""));
        boolean hasDetail = !detail.isEmpty();

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setBackgroundColor(rowIndex % 2 == 0 ? COLOR_ROW_EVEN : COLOR_ROW_ODD);
        table.addView(row, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.CENTER_VERTICAL);
        line.setMinimumHeight(dp(44));
        line.setPadding(dp(8), dp(6), dp(8), dp(6));
        row.addView(line, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        TextView indicatorView = makeText(hasDetail ? INDICATOR_COLLAPSED : "", 12, COLOR_INFO, false);
        line.addView(indicatorView, new LinearLayout.LayoutParams(
                dp(16),
                LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        TextView nameView = makeText(checkName, 12, COLOR_TEXT, false);
        line.addView(nameView, new LinearLayout.LayoutParams(
                0,
                LinearLayout.LayoutParams.WRAP_CONTENT,
                1f
        ));

        TextView statusChip = makeText(status, 12, statusColor(status), true);
        statusChip.setGravity(Gravity.END);
        statusChip.setMinWidth(dp(56));
        LinearLayout.LayoutParams chipParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        chipParams.leftMargin = dp(8);
        line.addView(statusChip, chipParams);

        if (!hasDetail) {
            return;
        }

        TextView detailView = makeText(detail, 11, COLOR_DETAIL, false);
        detailView.setPadding(dp(24), 0, dp(8), dp(8));
        detailView.setVisibility(View.GONE);
        row.addView(detailView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        row.setOnClickListener(view -> {
            boolean expanded = detailView.getVisibility() == View.VISIBLE;
            detailView.setVisibility(expanded ? View.GONE : View.VISIBLE);
            indicatorView.setText(expanded ? INDICATOR_COLLAPSED : INDICATOR_EXPANDED);
        });
    }

    /** Adds one initially-collapsed capability matrix for every reported target. */
    private void renderFormatCapabilities(JSONObject formatCapabilities) {
        addText("Format capabilities", 14, COLOR_TEXT, true, dp(16));
        if (formatCapabilities == null) {
            addText("Format capability table unavailable.", 12, COLOR_INFO, false, dp(4));
            return;
        }

        JSONArray capabilities = formatCapabilities.optJSONArray("capabilities");
        JSONArray targets = formatCapabilities.optJSONArray("targets");
        if (capabilities == null || capabilities.length() == 0 || targets == null || targets.length() == 0) {
            addText("Format capability table is empty.", 12, COLOR_INFO, false, dp(4));
            return;
        }

        for (int targetIndex = 0; targetIndex < targets.length(); ++targetIndex) {
            JSONObject target = targets.optJSONObject(targetIndex);
            if (target != null) {
                addFormatTargetTable(target, capabilities);
            }
        }
    }

    /**
     * Adds a target header whose table is created only while expanded. Removing the
     * table again on collapse avoids retaining all status cells for both backends.
     */
    private void addFormatTargetTable(JSONObject target, JSONArray capabilities) {
        String targetName = target.optString("name", "Unknown target");
        JSONArray rows = target.optJSONArray("rows");
        int formatCount = rows == null ? 0 : rows.length();

        LinearLayout section = new LinearLayout(this);
        section.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams sectionParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        sectionParams.topMargin = dp(6);
        contentLayout.addView(section, sectionParams);

        TextView toggleView = makeText(
                INDICATOR_COLLAPSED + " " + targetName + " (" + formatCount + " formats)",
                12,
                COLOR_TEXT,
                true
        );
        toggleView.setBackgroundColor(COLOR_ROW_EVEN);
        toggleView.setGravity(Gravity.CENTER_VERTICAL);
        toggleView.setMinimumHeight(dp(44));
        toggleView.setPadding(dp(10), dp(6), dp(10), dp(6));
        section.addView(toggleView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        ));

        toggleView.setOnClickListener(view -> {
            boolean expanded = section.getChildCount() > 1;
            if (expanded) {
                section.removeViews(1, section.getChildCount() - 1);
                toggleView.setText(
                        INDICATOR_COLLAPSED + " " + targetName + " (" + formatCount + " formats)"
                );
                return;
            }

            section.addView(buildFormatCapabilityTable(capabilities, rows), new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
            ));
            toggleView.setText(
                    INDICATOR_EXPANDED + " " + targetName + " (" + formatCount + " formats)"
            );
        });
    }

    /** Builds the horizontally-scrollable table for one target. */
    private HorizontalScrollView buildFormatCapabilityTable(JSONArray capabilities, JSONArray rows) {
        HorizontalScrollView scrollView = new HorizontalScrollView(this);
        scrollView.setFillViewport(false);
        scrollView.setHorizontalScrollBarEnabled(true);
        scrollView.setPadding(0, dp(2), 0, dp(4));

        LinearLayout table = new LinearLayout(this);
        table.setOrientation(LinearLayout.VERTICAL);
        table.addView(buildFormatCapabilityHeader(capabilities));

        if (rows != null) {
            for (int rowIndex = 0; rowIndex < rows.length(); ++rowIndex) {
                JSONArray row = rows.optJSONArray(rowIndex);
                if (row != null) {
                    table.addView(buildFormatCapabilityRow(capabilities.length(), row));
                }
            }
        }

        scrollView.addView(table, new HorizontalScrollView.LayoutParams(
                HorizontalScrollView.LayoutParams.WRAP_CONTENT,
                HorizontalScrollView.LayoutParams.WRAP_CONTENT
        ));
        return scrollView;
    }

    private LinearLayout buildFormatCapabilityHeader(JSONArray capabilities) {
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        addFormatTableCell(header, "Format", FORMAT_COLUMN_WIDTH_DP, COLOR_TABLE_HEADER, COLOR_TEXT, true);
        for (int capabilityIndex = 0; capabilityIndex < capabilities.length(); ++capabilityIndex) {
            addFormatTableCell(
                    header,
                    capabilities.optString(capabilityIndex, "Capability " + capabilityIndex),
                    CAPABILITY_COLUMN_WIDTH_DP,
                    COLOR_TABLE_HEADER,
                    COLOR_TEXT,
                    true
            );
        }
        return header;
    }

    private LinearLayout buildFormatCapabilityRow(int capabilityCount, JSONArray row) {
        LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);

        addFormatTableCell(
                line,
                row.optString(0, "Unknown"),
                FORMAT_COLUMN_WIDTH_DP,
                COLOR_FORMAT_CELL,
                COLOR_TEXT,
                false
        );

        long fullMask = row.optLong(1, 0L);
        long caveatMask = row.optLong(2, 0L);
        for (int capabilityIndex = 0; capabilityIndex < capabilityCount; ++capabilityIndex) {
            long capabilityBit = capabilityIndex < Long.SIZE ? 1L << capabilityIndex : 0L;
            boolean full = capabilityBit != 0L && (fullMask & capabilityBit) != 0L;
            boolean caveat = !full && capabilityBit != 0L && (caveatMask & capabilityBit) != 0L;
            addFormatTableCell(
                    line,
                    full ? "Full" : caveat ? "Caveat" : "None",
                    CAPABILITY_COLUMN_WIDTH_DP,
                    full ? COLOR_CAPABILITY_FULL : caveat ? COLOR_CAPABILITY_CAVEAT : COLOR_CAPABILITY_NONE,
                    caveat ? 0xFF000000 : 0xFFFFFFFF,
                    true
            );
        }
        return line;
    }

    private void addFormatTableCell(LinearLayout row,
                                    String text,
                                    int widthDp,
                                    int backgroundColor,
                                    int textColor,
                                    boolean bold) {
        TextView cell = makeText(text, 10, textColor, bold);
        cell.setBackgroundColor(backgroundColor);
        cell.setGravity(Gravity.CENTER);
        cell.setSingleLine(true);
        cell.setPadding(dp(6), 0, dp(6), 0);

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                dp(widthDp),
                dp(CAPABILITY_ROW_HEIGHT_DP)
        );
        params.rightMargin = dp(1);
        params.bottomMargin = dp(1);
        row.addView(cell, params);
    }

    /** Adds the collapsed "Raw report" toggle plus the (initially hidden) raw JSON dump. */
    private void addRawJsonSection(String json) {
        TextView toggleView = addText(INDICATOR_COLLAPSED + " Raw report (tap to expand)", 12, COLOR_INFO, true, dp(24));
        toggleView.setMinimumHeight(dp(44));
        toggleView.setGravity(Gravity.CENTER_VERTICAL);

        TextView rawView = addText(json, 10, COLOR_INFO, false, dp(4));
        rawView.setVisibility(View.GONE);

        toggleView.setOnClickListener(view -> {
            boolean expanded = rawView.getVisibility() == View.VISIBLE;
            rawView.setVisibility(expanded ? View.GONE : View.VISIBLE);
            toggleView.setText(expanded
                    ? INDICATOR_COLLAPSED + " Raw report (tap to expand)"
                    : INDICATOR_EXPANDED + " Raw report (tap to collapse)");
        });
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
        TextView view = makeText(text, sizeSp, color, bold);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.topMargin = topMarginPx;
        contentLayout.addView(view, params);
        return view;
    }

    /** Builds a styled TextView without attaching it to any parent. */
    private TextView makeText(String text, int sizeSp, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTypeface(Typeface.MONOSPACE, bold ? Typeface.BOLD : Typeface.NORMAL);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static native String nativeRunDriverPost();
}
