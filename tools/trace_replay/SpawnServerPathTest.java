package top.mobilegl.plugin.trace;

import java.io.File;

/**
 * Host-side checks for the split arms' MOBILEGL_IPC_SERVER_PATH resolution.
 *
 * <p>The only lane that used to run this parse end to end was an on-device spawn retrace, which
 * is slow, indirect, and reports a parse bug as "the knob had no effect" - so a defect in it
 * survived from P6 to P7. Run by tools/trace_replay/test_android_lifecycle.py with plain javac,
 * beside the TraceReplaySession lifecycle checks.
 *
 * <p>Deliberately assert-free, for the same reason trace_env_overrides_test.cpp is: -ea is not
 * on by default and a check compiled away is a green run that checked nothing.
 */
public final class SpawnServerPathTest {
    private static int failures = 0;

    private static final String DIR = "/data/app/~~hash==/top.mobilegl.plugin.trace-hash==/lib/arm64";
    private static final String RESOLVED =
            "MOBILEGL_IPC_SERVER_PATH=" + new File(DIR, "libMobileGLServer.so").getAbsolutePath();

    private static void expect(String what, String actual, String expected) {
        if (expected.equals(actual)) {
            return;
        }
        ++failures;
        System.err.println(what + "\n  got:      [" + actual + "]\n  expected: [" + expected + "]");
    }

    private static void expectTransport(String entries, String expected) {
        String actual = SpawnServerPath.effectiveTransport(entries);
        if (expected == null ? actual == null : expected.equals(actual)) {
            return;
        }
        ++failures;
        System.err.println("effectiveTransport(\"" + entries + "\") gave [" + actual
                + "], expected [" + expected + "]");
    }

    public static void main(String[] args) {
        // ---- the shapes the lanes really pass -------------------------------------------------
        // .github/workflows/apk.yml's spawn acceptance lane, verbatim.
        String spawnLane = "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_ROLE_SPLIT_STATE=1;"
                + "MOBILEGL_IPC_STRICT_ERRORS=1;MOBILEGL_IPC_RUN_AHEAD=1";
        expect("the spawn acceptance lane still gets its server path appended",
                SpawnServerPath.resolve(spawnLane, DIR), spawnLane + ";" + RESOLVED);
        // The inproc lane runs the server role on a thread HERE; a server path would be an unused
        // variable that made the two arms' environments differ for no reason.
        String inprocLane = "MOBILEGL_TRANSPORT=inproc;MOBILEGL_IPC_RUN_AHEAD=1";
        expect("inproc gets no server path", SpawnServerPath.resolve(inprocLane, DIR), inprocLane);
        expect("monolith (an empty list) gets no server path",
                SpawnServerPath.resolve("", DIR), "");
        expect("spawn alone still gets one",
                SpawnServerPath.resolve("MOBILEGL_TRANSPORT=spawn", DIR),
                "MOBILEGL_TRANSPORT=spawn;" + RESOLVED);
        expect("no nativeLibraryDir means nothing to resolve",
                SpawnServerPath.resolve(spawnLane, ""), spawnLane);

        // ---- an explicit value wins, in BOTH of its spellings ----------------------------------
        String explicit = "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_SERVER_PATH=/data/local/tmp/mine.so";
        expect("an explicit path is left alone", SpawnServerPath.resolve(explicit, DIR), explicit);
        // THE DEFECT THIS FILE EXISTS FOR. `KEY` with no '=' is the documented UNSET
        // (trace_env_overrides_test.cpp:92) and the only way to ask for "spawn with no image" and
        // see LaunchServer refuse by name. contains("MOBILEGL_IPC_SERVER_PATH=") did not match
        // it, so a path was appended after the unset and - last entry wins - undid it.
        String unset = "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_SERVER_PATH";
        expect("the unset spelling is honoured, not overwritten",
                SpawnServerPath.resolve(unset, DIR), unset);
        String emptied = "MOBILEGL_TRANSPORT=spawn;MOBILEGL_IPC_SERVER_PATH=";
        expect("an empty value is a value, and is left alone",
                SpawnServerPath.resolve(emptied, DIR), emptied);

        // ---- entry boundaries ------------------------------------------------------------------
        // The key inside ANOTHER entry's value said "the caller set it" and suppressed the
        // injection, so the spawn arm died on an unresolvable image for a log path's sake.
        String insideValue = "MOBILEGL_TRANSPORT=spawn;"
                + "MOBILEGL_LOG_FILE_PATH=/sdcard/MOBILEGL_IPC_SERVER_PATH=x.log";
        expect("a key inside another entry's value does not suppress the injection",
                SpawnServerPath.resolve(insideValue, DIR), insideValue + ";" + RESOLVED);
        // And the mirror image: nothing asked for spawn here.
        String transportInsideValue =
                "MOBILEGL_TRACE_NOTE=ran MOBILEGL_TRANSPORT=spawn yesterday";
        expect("a transport named inside another entry's value does not request spawn",
                SpawnServerPath.resolve(transportInsideValue, DIR), transportInsideValue);
        expect("a longer key that merely starts with the transport key is a different key",
                SpawnServerPath.resolve("MOBILEGL_TRANSPORT_NOTE=spawn", DIR),
                "MOBILEGL_TRANSPORT_NOTE=spawn");

        // ---- the library's own parsing rules ---------------------------------------------------
        expectTransport("", null);
        expectTransport("MOBILEGL_TRANSPORT=spawn", "spawn");
        // ConfigLoader.cpp:322 lower-cases the value before matching it, so this IS a spawn run
        // and it used to get no server path at all.
        expectTransport("MOBILEGL_TRANSPORT=Spawn", "spawn");
        expect("a capitalised value is still a spawn run",
                SpawnServerPath.resolve("MOBILEGL_TRANSPORT=Spawn", DIR),
                "MOBILEGL_TRANSPORT=Spawn;" + RESOLVED);
        // ApplyEnvOverrides applies entries in order with setenv(..., 1): the last one wins.
        expectTransport("MOBILEGL_TRANSPORT=inproc;MOBILEGL_TRANSPORT=spawn", "spawn");
        expectTransport("MOBILEGL_TRANSPORT=spawn;MOBILEGL_TRANSPORT=inproc", "inproc");
        expect("a later inproc really is the arm, so no server path is added",
                SpawnServerPath.resolve("MOBILEGL_TRANSPORT=spawn;MOBILEGL_TRANSPORT=inproc", DIR),
                "MOBILEGL_TRANSPORT=spawn;MOBILEGL_TRANSPORT=inproc");
        // A bare key is an unset, so the library falls back to its monolith default.
        expectTransport("MOBILEGL_TRANSPORT=spawn;MOBILEGL_TRANSPORT", null);
        // Empty entries are dropped and an empty key never reaches setenv.
        expectTransport(";;MOBILEGL_TRANSPORT=spawn;;", "spawn");
        expectTransport("=spawn", null);
        // Only the FIRST '=' separates, so a value may contain one.
        expectTransport("MOBILEGL_TRANSPORT=spawn=extra", "spawn=extra");

        // LAST, so that running this file against the pre-fix implementation reports the parsing
        // defects above rather than dying on this one line first.
        expect("a null list is not an NPE", SpawnServerPath.resolve(null, DIR), "");

        if (failures != 0) {
            System.err.println("SpawnServerPath: " + failures + " check(s) failed");
            System.exit(1);
        }
        System.out.println("SpawnServerPath: all checks passed");
    }
}
