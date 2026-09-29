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
import android.widget.CheckBox;
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
 * Control screen for the render server, in either shape: the offscreen supervisor
 * ({@link MobileGLServerService}) or the on-screen display server
 * ({@link MobileGLDisplayActivity}), which renders a client's window surface onto this
 * screen. Collects the shared {@code listen}/{@code token}/{@code env} extras and starts
 * or stops the selected one; no native code of its own. The endpoint is a tcp://host:port
 * for remote clients, or an @abstract name / filesystem path for on-device ones. One
 * server per device (P12 D8): the two shapes replace each other.
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
    private CheckBox onScreenBox;
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
        setContentView(NavBar.wrap(this, scrollView, NavBar.TAB_SERVER));

        contentLayout.addView(makeText("MobileGL Render Server", 20, COLOR_TEXT, true));

        TextView hint = makeText(
                "Starts the render server. Offscreen (default): the supervisor "
                        + "(libMobileGLServer.so --serve) runs as a foreground service. On-screen "
                        + "window: the server runs in a display Activity that renders the client's "
                        + "frames onto this screen (clients opt in with MOBILEGL_IPC_SURFACE=server). "
                        + "Endpoint forms: tcp://host:port for remote clients, @name for an "
                        + "on-device abstract socket, or a filesystem path (relative to the app's "
                        + "files dir) for on-device clients. Non-loopback TCP requires a token of at "
                        + "least " + MIN_TOKEN_BYTES + " bytes; unix endpoints need none, but GL programs "
                        + "from other apps (Termux, adb shell) reach the server through its broker, which "
                        + "needs one (see the launch command below). Extra env "
                        + "uses the KEY=VALUE;KEY grammar (e.g. MOBILEGL_BACKEND_TYPE=DirectVulkan). "
                        + "One server per device: the two modes replace each other.",
                12, COLOR_INFO, false);
        LinearLayout.LayoutParams hintParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        hintParams.topMargin = dp(8);
        contentLayout.addView(hint, hintParams);

        listenField = addField(contentLayout, "Listen endpoint", DEFAULT_ENDPOINT, dp(16));
        tokenField = addField(contentLayout, "Auth token (empty = loopback only)", "", dp(8));
        envField = addField(contentLayout, "Extra env (optional)", "", dp(8));

        onScreenBox = new CheckBox(this);
        onScreenBox.setText("On-screen window (render client frames onto this screen)");
        onScreenBox.setTextColor(COLOR_TEXT);
        onScreenBox.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        LinearLayout.LayoutParams boxParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        boxParams.topMargin = dp(8);
        contentLayout.addView(onScreenBox, boxParams);

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

        addExternalClientSection(contentLayout);

        if (isServerRunning()) {
            setStatus("A server is running (started outside this screen).", COLOR_WARN);
        } else {
            setStatus("Stopped.", COLOR_INFO);
        }

        handleIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    /**
     * Intent extras prefill the form, so adb can drive the screen without the IME:
     * --es listen @name --es token T --es env "K=V;K" --ez onscreen true --ez start true
     * (--ez stop true instead stops). This mirrors how every other entry point in this
     * APK is already driven by extras.
     */
    private void handleIntent(Intent intent) {
        if (intent != null) {
            String prefillListen = intent.getStringExtra("listen");
            if (prefillListen != null) listenField.setText(prefillListen);
            String prefillToken = intent.getStringExtra("token");
            if (prefillToken != null) tokenField.setText(prefillToken);
            String prefillEnv = intent.getStringExtra("env");
            if (prefillEnv != null) envField.setText(prefillEnv);
            if (intent.getBooleanExtra("onscreen", false)) onScreenBox.setChecked(true);
            if (intent.getBooleanExtra("stop", false)) stopServer();
            else if (intent.getBooleanExtra("start", false)) startServer();
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
        if (isServerRunning()) {
            setStatus("A server is already running; stop it before reconfiguring.", COLOR_WARN);
            return;
        }

        final boolean onScreen = onScreenBox.isChecked();
        Intent intent = new Intent(this,
                onScreen ? MobileGLDisplayActivity.class : MobileGLServerService.class);
        intent.putExtra("listen", endpoint);
        intent.putExtra("token", token);
        if (!env.isEmpty()) intent.putExtra("env", env);
        try {
            // The display Activity runs the server in its own process (:mglwin) and shows the
            // client's window surface; launching it brings that window to the front.
            if (onScreen) startActivity(intent);
            else startForegroundService(intent);
        } catch (RuntimeException error) {
            Log.e(TAG, "cannot start the render server", error);
            setStatus("start failed: " + error, COLOR_FAIL);
            return;
        }
        setStatus("Starting " + endpoint + (onScreen ? " (on-screen)" : "") + " ...", COLOR_INFO);
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
        boolean displayKilled = killDisplayServer();
        setStatus(stopped || displayKilled ? "Stopped." : "Stop requested, but no server was running.",
                stopped || displayKilled ? COLOR_INFO : COLOR_WARN);
    }

    /**
     * Kills the on-screen server's process (:mglwin). Same uid, so killProcess is allowed;
     * this mirrors MobileGLServerService.stopDisplayServer for the reverse direction.
     */
    private boolean killDisplayServer() {
        ActivityManager activities = getSystemService(ActivityManager.class);
        String displayProcess = getPackageName() + ":mglwin";
        List<ActivityManager.RunningAppProcessInfo> processes = activities.getRunningAppProcesses();
        if (processes == null) return false;
        for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (displayProcess.equals(process.processName)) {
                android.os.Process.killProcess(process.pid);
                return true;
            }
        }
        return false;
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
                if (isServerRunning()) {
                    setStatus("Server running but " + display + " did not accept a probe; "
                            + "see logcat tags MobileGLServer/MobileGLDisplay.", COLOR_WARN);
                } else {
                    setStatus("Server exited; see logcat tags MobileGLServer/MobileGLDisplay.",
                            COLOR_FAIL);
                }
            });
        }, "mgl-server-probe");
        probeThread = thread;
        thread.start();
    }

    /** True when either server shape is up: the supervisor service or the :mglwin process. */
    private boolean isServerRunning() {
        ActivityManager activities = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningServiceInfo> services =
                activities.getRunningServices(Integer.MAX_VALUE);
        if (services != null) {
            for (ActivityManager.RunningServiceInfo service : services) {
                if (MobileGLServerService.class.getName().equals(service.service.getClassName())) {
                    return true;
                }
            }
        }
        String displayProcess = getPackageName() + ":mglwin";
        List<ActivityManager.RunningAppProcessInfo> processes = activities.getRunningAppProcesses();
        if (processes != null) {
            for (ActivityManager.RunningAppProcessInfo process : processes) {
                if (displayProcess.equals(process.processName)) return true;
            }
        }
        return false;
    }

    // ---- P11 B1 (CONTRACT-P11 B1): the launch command for same-device external clients ----------
    //
    // A GL program started from another app (Termux, adb shell) cannot connect to this app's unix
    // endpoint (SELinux), so it runs ExternalClientHelper under app_process with THIS APK as its class
    // path; the helper asks ExternalClientBroker for a connected pair and execs the program on it. The
    // program's side needs three facts it cannot look up itself - package visibility filters `pm path`
    // for an app, and a native program has no Context - so this screen prints them: the APK path, the
    // helper class, the broker component, and how the token travels (the environment, never argv).

    private TextView launchCommandView;

    private void addExternalClientSection(LinearLayout parent) {
        TextView title = makeText("Same-device GL programs (Termux, adb shell, other apps)", 14, COLOR_TEXT, true);
        LinearLayout.LayoutParams titleParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        titleParams.topMargin = dp(20);
        parent.addView(title, titleParams);

        launchCommandView = makeText("", 12, COLOR_INFO, false);
        launchCommandView.setTextIsSelectable(true);
        LinearLayout.LayoutParams commandParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        commandParams.topMargin = dp(8);
        parent.addView(launchCommandView, commandParams);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rowParams.topMargin = dp(8);
        parent.addView(row, rowParams);

        Button generate = new Button(this);
        generate.setText("Generate token");
        generate.setAllCaps(false);
        generate.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        generate.setOnClickListener(v -> {
            byte[] bytes = new byte[16];
            new java.security.SecureRandom().nextBytes(bytes);
            StringBuilder hex = new StringBuilder();
            for (byte b : bytes) hex.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            tokenField.setText(hex.toString());
        });
        row.addView(generate);

        Button copy = new Button(this);
        copy.setText("Copy launch command");
        copy.setAllCaps(false);
        copy.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        LinearLayout.LayoutParams copyParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        copyParams.leftMargin = dp(8);
        row.addView(copy, copyParams);
        copy.setOnClickListener(v -> {
            android.content.ClipboardManager clipboard = getSystemService(android.content.ClipboardManager.class);
            clipboard.setPrimaryClip(android.content.ClipData.newPlainText("MobileGL launch command",
                    launchCommand(tokenField.getText().toString())));
            setStatus("Launch command copied (it carries the token: paste it only where you run the program).",
                    COLOR_INFO);
        });

        tokenField.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(android.text.Editable s) { refreshLaunchCommand(); }
        });
        refreshLaunchCommand();
    }

    /** The one line a user pastes before their program: token in the environment, the APK as class path. */
    private String launchCommand(String token) {
        return "MOBILEGL_IPC_TOKEN='" + token + "' CLASSPATH=" + getApplicationInfo().sourceDir
                + " app_process / " + ExternalClientHelper.class.getName() + " -- <program> [args...]";
    }

    private void refreshLaunchCommand() {
        if (launchCommandView == null) return;
        String token = tokenField.getText().toString();
        boolean usable = token.getBytes().length >= ExternalClientBroker.MIN_TOKEN_BYTES;
        StringBuilder text = new StringBuilder();
        if (!usable) {
            text.append("Needs an Auth token of at least ").append(ExternalClientBroker.MIN_TOKEN_BYTES)
                    .append(" bytes (Generate token), then start the server offscreen.\n\n");
        }
        text.append(launchCommand(usable ? token : "<token>")).append("\n\n")
                .append("Broker: ").append(getPackageName()).append('/').append(ExternalClientBroker.class.getName())
                .append("\nThe token travels in the environment (MOBILEGL_IPC_TOKEN, or MOBILEGL_IPC_TOKEN_FILE=<file>)")
                .append(", never on the command line. The program runs over shared memory; the server keeps")
                .append(" running as a foreground service. Battery: set both this app and the client's app to")
                .append(" \"No restrictions\".");
        launchCommandView.setText(text.toString());
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
