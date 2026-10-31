package top.mobilegl.plugin.trace;

import java.io.File;
import java.util.Locale;

/**
 * Resolves {@code MOBILEGL_IPC_SERVER_PATH} for the split arms, from the one process that can
 * spell it.
 *
 * <p>P6: {@code MOBILEGL_TRANSPORT=spawn} NEEDS AN ABSOLUTE PATH TO AN EXEC-ABLE FILE, and on
 * Android there is exactly one such place. An APK's {@code lib/<abi>/} is the only directory an
 * untrusted_app may exec from - W^X has forbidden the app's own data dirs since API 29 - and the
 * packager only puts a file there if it is named {@code lib*.so}, which is why the server
 * executable wears that name (P0 spike A proved the arrangement on two devices).
 *
 * <p>NOTHING OFF THE DEVICE CAN SPELL THAT PATH. {@code nativeLibraryDir} contains an
 * install-time hash, so the host-side runner cannot pass {@code MOBILEGL_IPC_SERVER_PATH} the way
 * it passes every other knob; the path has to be resolved HERE, by the process that is about to
 * launch the server. Left unresolved, {@code LaunchServer} refuses BY NAME and the arm reds on
 * "unresolvable image" - honest, but about the wrong thing.
 *
 * <p>AN EXPLICIT VALUE WINS. A caller that spoke about {@code MOBILEGL_IPC_SERVER_PATH} itself is
 * pointing somewhere deliberately - or deliberately pointing NOWHERE - and silently replacing
 * that would make the knob untestable.
 *
 * <p>This lives in a class of its own, with no {@code android.*} import, so that
 * {@code tools/trace_replay/test_android_lifecycle.py} can compile and run it with plain javac.
 * It is a hand-rolled parse of the same {@code K=V;K=V} string the native side parses, and the
 * only lane that used to exercise it end to end was an on-device spawn retrace - which reports a
 * parse bug as "the knob had no effect", the hardest shape to notice there is.
 *
 * <p>THE RULES ARE THE NATIVE SIDE'S, NOT A SECOND SET. They are the ones pinned by
 * {@code tools/trace_replay/trace_env_overrides_test.cpp} and applied by
 * {@code trace_replay_core.cpp:143} {@code ApplyEnvOverrides}:
 * <ul>
 *   <li>the list is split on {@code ';'} and empty entries are dropped;</li>
 *   <li>the FIRST {@code '='} separates, so a value may contain {@code '='};</li>
 *   <li>an entry with NO {@code '='} is an UNSET, not a key with an empty value;</li>
 *   <li>an empty key is ignored;</li>
 *   <li>entries are applied in order with {@code setenv(..., 1)}, so the LAST one wins;</li>
 *   <li>and ConfigLoader lower-cases {@code MOBILEGL_TRANSPORT} before matching it
 *       ({@code ConfigLoader.cpp:322}), so the comparison here is case-insensitive too.</li>
 * </ul>
 */
final class SpawnServerPath {
    static final String TRANSPORT_KEY = "MOBILEGL_TRANSPORT";
    static final String SERVER_PATH_KEY = "MOBILEGL_IPC_SERVER_PATH";
    static final String SERVER_LIBRARY = "libMobileGLServer.so";

    private SpawnServerPath() {
    }

    /**
     * Returns {@code envOverrides} with the resolved server path appended, or unchanged.
     *
     * <p>ONLY {@code spawn} GETS A PATH, and that is not an oversight: it is the only transport
     * that execs an image. {@code monolith} runs no server role at all and {@code inproc} runs it
     * on a thread of this process, so a server path there would be an unused variable that made
     * the two arms differ in their environment for no reason.
     */
    static String resolve(String envOverrides, String nativeLibraryDir) {
        String entries = envOverrides == null ? "" : envOverrides;
        if (nativeLibraryDir == null || nativeLibraryDir.isEmpty()) {
            return entries;
        }
        if (!"spawn".equals(effectiveTransport(entries)) || mentions(entries, SERVER_PATH_KEY)) {
            return entries;
        }
        String entry =
                SERVER_PATH_KEY + "=" + new File(nativeLibraryDir, SERVER_LIBRARY).getAbsolutePath();
        return entries.isEmpty() ? entry : entries + ";" + entry;
    }

    /**
     * The transport the LIBRARY will see: the last assignment wins, lower-cased, and a trailing
     * unset of the key means there is none.
     *
     * <p>Returns {@code null} when the list says nothing about the transport, which is how
     * "ConfigLoader falls back to its monolith default" is spelled here.
     */
    static String effectiveTransport(String envOverrides) {
        String value = null;
        for (String entry : split(envOverrides)) {
            int separator = entry.indexOf('=');
            if (separator < 0) {
                if (entry.equals(TRANSPORT_KEY)) {
                    value = null; // The documented unset spelling.
                }
                continue;
            }
            if (separator == 0) {
                continue; // An empty key never reaches setenv.
            }
            if (entry.substring(0, separator).equals(TRANSPORT_KEY)) {
                value = entry.substring(separator + 1);
            }
        }
        return value == null ? null : value.toLowerCase(Locale.ROOT);
    }

    /**
     * True when the caller named {@code key} at all - as an assignment OR as a bare unset.
     *
     * <p>Both spellings are the caller speaking. The unset one matters most: it is the only way
     * to ask for "spawn with no image" and see {@code LaunchServer} refuse by name, and appending
     * a resolved path after it would silently undo it, because the last entry wins.
     */
    static boolean mentions(String envOverrides, String key) {
        for (String entry : split(envOverrides)) {
            int separator = entry.indexOf('=');
            String name = separator < 0 ? entry : entry.substring(0, separator);
            if (!name.isEmpty() && name.equals(key)) {
                return true;
            }
        }
        return false;
    }

    private static String[] split(String envOverrides) {
        return envOverrides == null ? new String[0] : envOverrides.split(";", -1);
    }
}
