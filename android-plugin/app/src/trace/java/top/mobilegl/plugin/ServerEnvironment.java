package top.mobilegl.plugin;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The server role's environment, shared by the two servers the trace APK can run.
 *
 * <p>{@link MobileGLServerService} exec's the offscreen supervisor with it (a ProcessBuilder
 * environment); {@link MobileGLDisplayActivity} applies it to its OWN process with Os.setenv /
 * Os.unsetenv before libMobileGL is loaded, because the on-screen display server runs in that
 * process (P12 D7). One grammar and one set of forced keys for both, so the two servers cannot
 * drift apart on what an {@code env} extra means.
 *
 * <p>No android.* import: tools/trace_replay/test_android_lifecycle.py compiles and runs this class
 * with plain javac (ServerEnvironmentTest), which is the only place it runs outside a device.
 */
public final class ServerEnvironment {
    private ServerEnvironment() {}

    public static final String BACKEND = "MOBILEGL_BACKEND_TYPE";
    public static final String TOKEN = "MOBILEGL_IPC_TOKEN";
    public static final String LOG_FILE_PATH = "MOBILEGL_LOG_FILE_PATH";

    /**
     * What the server must never inherit: a transport (it would dial instead of serving), the
     * client's endpoint knobs, and the spawn arm's sizing (mobilegl_server_main's scrub list, exit
     * 65, plus the two endpoint names the listen extra replaces).
     */
    static final String[] SCRUBBED = {
            "MOBILEGL_TRANSPORT", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_ENDPOINT",
            "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB", "MOBILEGL_IPC_STAGE_MB",
    };

    /**
     * Applies the replay runner's {@code KEY=VALUE;KEY} grammar to {@code env}: {@code K=V} sets
     * (an empty V is a set, not an unset), a bare {@code K} unsets, empty entries and entries with
     * an empty key are skipped. The last entry for a key wins.
     */
    public static void apply(Map<String, String> env, String overrides) {
        if (overrides == null) return;
        for (String entry : overrides.split(";", -1)) {
            int separator = entry.indexOf('=');
            if (entry.isEmpty() || separator == 0) continue;
            if (separator < 0) env.remove(entry);
            else env.put(entry.substring(0, separator), entry.substring(separator + 1));
        }
    }

    /**
     * Turns {@code env} into the server role's environment, in place: the {@code env} extra first,
     * then an explicit backend pin (it wins over the extra's), then the keys every server forces -
     * ROLE=server, DIAL=no, LOG_FORWARD=1, the scrub list removed, the token (empty when none) -
     * and a log file path when none was given.
     */
    public static void applyServerRole(Map<String, String> env, String overrides, String token, String backend,
                                       String defaultLogPath) {
        apply(env, overrides);
        if (backend != null && !backend.isEmpty()) env.put(BACKEND, backend);
        env.put("MOBILEGL_IPC_ROLE", "server");
        env.put("MOBILEGL_IPC_DIAL", "no");
        env.put("MOBILEGL_IPC_LOG_FORWARD", "1");
        for (String name : SCRUBBED) env.remove(name);
        env.put(TOKEN, token == null ? "" : token);
        if (defaultLogPath != null) env.putIfAbsent(LOG_FILE_PATH, defaultLogPath);
    }

    /**
     * The edits that turn {@code before} into {@code after}, in a stable order: a key whose value
     * is new or different maps to that value, a key {@code after} dropped maps to null (unset).
     * The display Activity applies exactly these with Os.setenv / Os.unsetenv.
     */
    public static Map<String, String> changes(Map<String, String> before, Map<String, String> after) {
        Map<String, String> edits = new LinkedHashMap<>();
        for (Map.Entry<String, String> entry : new java.util.TreeMap<>(after).entrySet()) {
            if (!entry.getValue().equals(before.get(entry.getKey()))) edits.put(entry.getKey(), entry.getValue());
        }
        for (String name : new java.util.TreeSet<>(before.keySet())) {
            if (!after.containsKey(name)) edits.put(name, null);
        }
        return edits;
    }

    /**
     * Aspect-fit (letterbox / pillarbox): the largest {@code bufferWidth:bufferHeight} rectangle
     * inside a {@code containerWidth x containerHeight} view, as {width, height}. {-1, -1} (fill the
     * container, MATCH_PARENT) when either size is unknown or no fixed buffer size is set.
     */
    public static int[] aspectFit(int containerWidth, int containerHeight, int bufferWidth, int bufferHeight) {
        if (containerWidth <= 0 || containerHeight <= 0 || bufferWidth <= 0 || bufferHeight <= 0) {
            return new int[]{-1, -1};
        }
        long wide = (long) containerWidth * bufferHeight;
        long tall = (long) containerHeight * bufferWidth;
        if (wide <= tall) {
            // The container is narrower than the buffer: full width, bars above and below.
            return new int[]{containerWidth, (int) Math.max(1, wide / bufferWidth)};
        }
        // The container is wider: full height, bars left and right.
        return new int[]{(int) Math.max(1, tall / bufferHeight), containerHeight};
    }
}
