package top.mobilegl.plugin;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.text.InputType;
import android.util.Log;
import android.util.TypedValue;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.List;
import java.util.Locale;

/**
 * Control screen for the offscreen render server ({@link MobileGLServerService}).
 * Collects the service's {@code listen}/{@code token}/{@code env} extras and starts or
 * stops it; no native code of its own. The endpoint is a tcp://host:port for remote
 * clients, or an @abstract name / filesystem path for on-device ones. One server per
 * device (P12 D8): starting this service kills the on-screen display server in :mglwin,
 * and vice versa.
 */
public final class ServerControlActivity extends Activity {
    private static final String TAG = "MobileGLServerCtl";

    private static final int COLOR_BACKGROUND = 0xFF121212;
    private static final int COLOR_TEXT = 0xFFEEEEEE;
    private static final int COLOR_PASS = 0xFF4CAF50;
    private static final int COLOR_WARN = 0xFFFF9800;
    private static final int COLOR_FAIL = 0xFFF44336;
    private static final int COLOR_INFO = 0xFF9E9E9E;

    private static final String DEFAULT_ENDPOINT = "tcp://0.0.0.0:40613";
    private static final int MIN_TOKEN_BYTES = 16;
    private static final int PROBE_ATTEMPTS = 10;
    private static final long PROBE_INTERVAL_MS = 500;
    private static final int PROBE_TIMEOUT_MS = 500;

    private EditText listenField;
    private EditText tokenField;
    private EditText envField;
    private Button startButton;
    private Button stopButton;
    private TextView statusView;
    private volatile Thread probeThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout contentLayout = new LinearLayout(this);
        contentLayout.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(16);
        contentLayout.setPadding(padding, padding, padding, padding);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(COLOR_BACKGROUND);
        scrollView.setFillViewport(true);
        scrollView.addView(contentLayout);
        setContentView(scrollView);

        contentLayout.addView(makeText("MobileGL Render Server", 20, COLOR_TEXT, true));

        TextView hint = makeText(
                "Starts the offscreen render server (libMobileGLServer.so --serve) as a foreground "
                        + "service. Endpoint forms: tcp://host:port for remote clients, @name for an "
                        + "on-device abstract socket, or a filesystem path (relative to the app's "
                        + "files dir) for on-device clients. Non-loopback TCP requires a token of at "
                        + "least " + MIN_TOKEN_BYTES + " bytes; unix endpoints need none. Extra env "
                        + "uses the KEY=VALUE;KEY grammar (e.g. MOBILEGL_BACKEND_TYPE=DirectVulkan). "
                        + "One server per device: this replaces the on-screen display server.",
                12, COLOR_INFO, false);
        LinearLayout.LayoutParams hintParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        hintParams.topMargin = dp(8);
        contentLayout.addView(hint, hintParams);

        listenField = addField(contentLayout, "Listen endpoint", DEFAULT_ENDPOINT, dp(16));
        tokenField = addField(contentLayout, "Auth token (empty = loopback only)", "", dp(8));
        envField = addField(contentLayout, "Extra env (optional)", "", dp(8));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams buttonsParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        buttonsParams.topMargin = dp(16);
        contentLayout.addView(buttons, buttonsParams);

        startButton = new Button(this);
        startButton.setText("Start server");
        startButton.setAllCaps(false);
        startButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        startButton.setOnClickListener(v -> startServer());
        buttons.addView(startButton);

        stopButton = new Button(this);
        stopButton.setText("Stop server");
        stopButton.setAllCaps(false);
        stopButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        LinearLayout.LayoutParams stopParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        stopParams.leftMargin = dp(8);
        buttons.addView(stopButton, stopParams);
        stopButton.setOnClickListener(v -> stopServer());

        statusView = makeText("", 13, COLOR_INFO, false);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        statusParams.topMargin = dp(12);
        contentLayout.addView(statusView, statusParams);

        if (isServiceRunning()) {
            setStatus("Service is running (started outside this screen).", COLOR_WARN);
        } else {
            setStatus("Stopped.", COLOR_INFO);
        }
    }

    @Override
    protected void onDestroy() {
        Thread probe = probeThread;
        if (probe != null) probe.interrupt();
        super.onDestroy();
    }

    private void startServer() {
        String endpoint = listenField.getText().toString().trim();
        String token = tokenField.getText().toString();
        String env = envField.getText().toString().trim();

        String probeDisplay;
        Probe probe;
        if (endpoint.startsWith("tcp://")) {
            String[] hostPort = parseTcpEndpoint(endpoint);
            if (hostPort == null) {
                setStatus("Invalid TCP endpoint: expected tcp://host:port", COLOR_FAIL);
                return;
            }
            if (!isLoopback(hostPort[0]) && token.getBytes().length < MIN_TOKEN_BYTES) {
                setStatus("Non-loopback listen requires a token of at least "
                        + MIN_TOKEN_BYTES + " bytes.", COLOR_FAIL);
                return;
            }
            final String probeHost = isWildcard(hostPort[0]) ? "127.0.0.1" : hostPort[0];
            final int port;
            try {
                port = Integer.parseInt(hostPort[1]);
            } catch (NumberFormatException error) {
                setStatus("Invalid port in endpoint.", COLOR_FAIL);
                return;
            }
            probeDisplay = probeHost + ":" + port;
            probe = () -> {
                try (Socket socket = new Socket()) {
                    socket.connect(new InetSocketAddress(probeHost, port), PROBE_TIMEOUT_MS);
                }
            };
        } else {
            String invalid = validateUnixEndpoint(endpoint);
            if (invalid != null) {
                setStatus(invalid, COLOR_FAIL);
                return;
            }
            final boolean abstractName = endpoint.startsWith("@");
            final String address = abstractName ? endpoint.substring(1)
                    : endpoint.startsWith("/") ? endpoint
                    : new File(getFilesDir(), endpoint).getAbsolutePath();
            final LocalSocketAddress.Namespace namespace = abstractName
                    ? LocalSocketAddress.Namespace.ABSTRACT : LocalSocketAddress.Namespace.FILESYSTEM;
            probeDisplay = endpoint;
            probe = () -> {
                LocalSocket socket = new LocalSocket();
                try {
                    socket.connect(new LocalSocketAddress(address, namespace));
                } finally {
                    socket.close();
                }
            };
        }
        if (isServiceRunning()) {
            setStatus("Service already running; stop it before reconfiguring.", COLOR_WARN);
            return;
        }

        Intent intent = new Intent(this, MobileGLServerService.class);
        intent.putExtra("listen", endpoint);
        intent.putExtra("token", token);
        if (!env.isEmpty()) intent.putExtra("env", env);
        try {
            startForegroundService(intent);
        } catch (RuntimeException error) {
            Log.e(TAG, "cannot start MobileGLServerService", error);
            setStatus("startForegroundService failed: " + error, COLOR_FAIL);
            return;
        }
        setStatus("Starting " + endpoint + " ...", COLOR_INFO);
        probeUntilListening(probeDisplay, probe);
    }

    /**
     * unix endpoint check: an @abstract name or a filesystem path (relative paths resolve
     * against the app's files dir, which is the service's working directory). Returns the
     * problem, or null when the endpoint is usable. No token: an AF_UNIX address is reachable
     * only from this device, and an abstract name only from this app.
     */
    private String validateUnixEndpoint(String endpoint) {
        if (endpoint.isEmpty()) {
            return "Endpoint is empty.";
        }
        if (endpoint.contains("://")) {
            return "Unknown endpoint scheme; use tcp://host:port, @name or a filesystem path.";
        }
        String resolved = endpoint.startsWith("@") || endpoint.startsWith("/") ? endpoint
                : new File(getFilesDir(), endpoint).getAbsolutePath();
        // sun_path is 108 bytes on Linux/Android, and an abstract name drops the '@' into it.
        if (resolved.getBytes().length >= 108) {
            return "Endpoint is too long for an AF_UNIX address.";
        }
        return null;
    }

    private void stopServer() {
        Thread probe = probeThread;
        if (probe != null) probe.interrupt();
        boolean stopped = stopService(new Intent(this, MobileGLServerService.class));
        setStatus(stopped ? "Stopped." : "Stop requested, but the service was not running.",
                stopped ? COLOR_INFO : COLOR_WARN);
    }

    /**
     * Confirms the supervisor bound its socket: connect to the endpoint until one attempt
     * succeeds. TCP probes go to loopback when the server listens on a wildcard address;
     * unix probes use LocalSocket. The server closes each unauthenticated probe after its
     * pre-auth timeout; at most {@link #PROBE_ATTEMPTS} probes are made, then the screen
     * reports whatever the service state is.
     */
    private interface Probe {
        void connect() throws Exception;
    }

    private void probeUntilListening(String display, Probe probe) {
        Thread thread = new Thread(() -> {
            for (int attempt = 1; attempt <= PROBE_ATTEMPTS; ++attempt) {
                if (Thread.currentThread().isInterrupted()) return;
                try {
                    probe.connect();
                    runOnUiThread(() -> setStatus("Listening on " + display, COLOR_PASS));
                    return;
                } catch (Exception refused) {
                    try {
                        Thread.sleep(PROBE_INTERVAL_MS);
                    } catch (InterruptedException stopped) {
                        return;
                    }
                }
            }
            runOnUiThread(() -> {
                if (isServiceRunning()) {
                    setStatus("Service running but " + display
                            + " did not accept a probe; see logcat tag MobileGLServer.", COLOR_WARN);
                } else {
                    setStatus("Service exited; see logcat tag MobileGLServer.", COLOR_FAIL);
                }
            });
        }, "mgl-server-probe");
        probeThread = thread;
        thread.start();
    }

    private boolean isServiceRunning() {
        ActivityManager activities = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningServiceInfo> services =
                activities.getRunningServices(Integer.MAX_VALUE);
        if (services == null) return false;
        for (ActivityManager.RunningServiceInfo service : services) {
            if (MobileGLServerService.class.getName().equals(service.service.getClassName())) {
                return true;
            }
        }
        return false;
    }

    /** {host, port} for tcp://host:port, or null. */
    private static String[] parseTcpEndpoint(String endpoint) {
        if (endpoint == null || !endpoint.startsWith("tcp://")) return null;
        String hostPort = endpoint.substring("tcp://".length());
        int separator = hostPort.lastIndexOf(':');
        if (separator <= 0 || separator == hostPort.length() - 1) return null;
        return new String[]{hostPort.substring(0, separator), hostPort.substring(separator + 1)};
    }

    private static boolean isLoopback(String host) {
        // A wildcard bind is reachable from the network, so the server holds it to the
        // non-loopback token requirement (AuthToken.h) just like a real interface address.
        return host.equals("127.0.0.1") || host.equals("localhost") || host.equals("::1");
    }

    private static boolean isWildcard(String host) {
        return host.isEmpty() || host.equals("0.0.0.0") || host.equals("::") || host.equals("*");
    }

    private EditText addField(LinearLayout parent, String label, String initial, int topMarginPx) {
        TextView labelView = makeText(label, 12, COLOR_INFO, false);
        LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        labelParams.topMargin = topMarginPx;
        parent.addView(labelView, labelParams);

        EditText field = new EditText(this);
        field.setText(initial);
        field.setTextColor(COLOR_TEXT);
        field.setHintTextColor(COLOR_INFO);
        field.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        field.setTypeface(Typeface.MONOSPACE);
        field.setSingleLine(true);
        field.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        parent.addView(field, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return field;
    }

    private void setStatus(String message, int color) {
        Log.i(TAG, String.format(Locale.ROOT, "status: %s", message));
        statusView.setText(message);
        statusView.setTextColor(color);
    }

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
}
