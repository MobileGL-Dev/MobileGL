package top.mobilegl.plugin;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Host-side checks for ServerEnvironment: the server role's environment and the display's
 * aspect-fit, shared by MobileGLServerService (the offscreen supervisor's exec environment) and
 * MobileGLDisplayActivity (the on-screen display server's own process, P12 D7).
 *
 * <p>On a device a defect here reads as "the knob had no effect" or "the server refused to start"
 * (a scrubbed key left in: exit 65), so it is exercised here, by tools/trace_replay/
 * test_android_lifecycle.py with plain javac. Assert-free for the same reason as
 * SpawnServerPathTest: -ea is off by default, and a check compiled away checks nothing.
 */
public final class ServerEnvironmentTest {
    private static int failures = 0;

    private static void expect(String what, Object actual, Object expected) {
        if (expected == null ? actual == null : expected.equals(actual)) return;
        ++failures;
        System.err.println(what + "\n  got:      [" + actual + "]\n  expected: [" + expected + "]");
    }

    private static Map<String, String> map(String... keyValues) {
        Map<String, String> result = new LinkedHashMap<>();
        for (int i = 0; i < keyValues.length; i += 2) result.put(keyValues[i], keyValues[i + 1]);
        return result;
    }

    public static void main(String[] args) {
        // ---- the K=V;K grammar (the service's, unchanged) -------------------------------------------
        Map<String, String> env = new HashMap<>(map("B", "old", "E", "keep"));
        ServerEnvironment.apply(env, "A=1;B;C=;=x;;D=4;A=2");
        expect("K=V sets, a bare K unsets, K= sets empty, =x and empty entries are skipped, the last wins",
                env, new HashMap<>(map("A", "2", "C", "", "D", "4", "E", "keep")));
        Map<String, String> untouched = new HashMap<>(map("A", "1"));
        ServerEnvironment.apply(untouched, null);
        expect("no env extra changes nothing", untouched, new HashMap<>(map("A", "1")));

        // ---- the server role ----------------------------------------------------------------------
        Map<String, String> launcher = new HashMap<>(map(
                "MOBILEGL_TRANSPORT", "spawn",
                "MOBILEGL_IPC_CONTROL", "tcp://10.0.0.2:40613",
                "MOBILEGL_IPC_ENDPOINT", "tcp://x",
                "MOBILEGL_IPC_DIAL", "yes",
                "PATH", "/system/bin"));
        ServerEnvironment.applyServerRole(launcher,
                "MOBILEGL_IPC_ROLE=client;MOBILEGL_BACKEND_TYPE=DirectGLES;MOBILEGL_IPC_RING_MB=8;"
                        + "MOBILEGL_IPC_SERVER_PATH=/x;MOBILEGL_IPC_STAGE_MB=4;FOO=bar",
                "a-token-of-sixteen-bytes", "DirectVulkan", "/data/files/mglwin.log");
        expect("the server role: forced keys win over the env extra, the scrub list is gone, the backend "
                        + "extra pins over the env's, the token and the default log path are set",
                launcher, new HashMap<>(map(
                        "PATH", "/system/bin",
                        "FOO", "bar",
                        "MOBILEGL_BACKEND_TYPE", "DirectVulkan",
                        "MOBILEGL_IPC_ROLE", "server",
                        "MOBILEGL_IPC_DIAL", "no",
                        "MOBILEGL_IPC_LOG_FORWARD", "1",
                        "MOBILEGL_IPC_TOKEN", "a-token-of-sixteen-bytes",
                        "MOBILEGL_LOG_FILE_PATH", "/data/files/mglwin.log")));
        Map<String, String> service = new HashMap<>();
        ServerEnvironment.applyServerRole(service, "MOBILEGL_BACKEND_TYPE=DirectGLES;MOBILEGL_LOG_FILE_PATH=/sdcard/x.log",
                null, null, "/data/files/mgl.log");
        expect("the service's shape: no token is an empty token, no backend extra keeps the env's, an env "
                        + "log path is kept",
                service, new HashMap<>(map(
                        "MOBILEGL_BACKEND_TYPE", "DirectGLES",
                        "MOBILEGL_IPC_ROLE", "server",
                        "MOBILEGL_IPC_DIAL", "no",
                        "MOBILEGL_IPC_LOG_FORWARD", "1",
                        "MOBILEGL_IPC_TOKEN", "",
                        "MOBILEGL_LOG_FILE_PATH", "/sdcard/x.log")));

        // ---- the Os.setenv / Os.unsetenv edits the display Activity applies ---------------------------
        Map<String, String> edits = ServerEnvironment.changes(
                map("A", "1", "B", "2", "C", "3", "MOBILEGL_TRANSPORT", "spawn"),
                map("A", "1", "B", "5", "D", "4", "E", ""));
        expect("changed and new keys are set (sorted), dropped keys are unset (null), equal keys are left",
                new java.util.ArrayList<>(edits.entrySet()).toString(),
                "[B=5, D=4, E=, C=null, MOBILEGL_TRANSPORT=null]");

        // ---- aspect-fit ----------------------------------------------------------------------------------
        expect("a portrait screen letterboxes a 640x480 buffer: full width",
                java.util.Arrays.toString(ServerEnvironment.aspectFit(1080, 2400, 640, 480)), "[1080, 810]");
        expect("a landscape screen pillarboxes it: full height",
                java.util.Arrays.toString(ServerEnvironment.aspectFit(2400, 1080, 640, 480)), "[1440, 1080]");
        expect("the same aspect fills the view",
                java.util.Arrays.toString(ServerEnvironment.aspectFit(1280, 960, 640, 480)), "[1280, 960]");
        expect("no fixed size (0/0: the layout's own) fills the view",
                java.util.Arrays.toString(ServerEnvironment.aspectFit(1080, 2400, 0, 0)), "[-1, -1]");
        expect("an unmeasured view fills the view",
                java.util.Arrays.toString(ServerEnvironment.aspectFit(0, 0, 640, 480)), "[-1, -1]");

        if (failures != 0) {
            System.err.println("ServerEnvironmentTest: " + failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("ServerEnvironmentTest: all checks passed");
    }
}
