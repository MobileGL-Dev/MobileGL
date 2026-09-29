package top.mobilegl.plugin;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import java.io.File;
import java.io.FileDescriptor;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * P11 B1 (CONTRACT-P11 B1): how a native GL program with no Java {@code Context} - started from
 * Termux, adb shell or another app - reaches the render server over shared memory on the same
 * device. Run with {@code app_process} under the program's own uid, from the server's APK:
 *
 * <pre>
 *   MOBILEGL_IPC_TOKEN=&lt;token&gt; CLASSPATH=&lt;server APK&gt; app_process / top.mobilegl.plugin.ExternalClientHelper \
 *       [--server &lt;package&gt;] [--timeout-ms &lt;n&gt;] [--] &lt;program&gt; [args...]
 * </pre>
 *
 * <p>The token is read from {@code MOBILEGL_IPC_TOKEN}, or from the file {@code MOBILEGL_IPC_TOKEN_FILE}
 * names - NEVER from argv, which every process on the device can read. {@code --server} defaults to
 * the package of the APK this class was loaded from.
 *
 * <p>The helper builds a Binder, sends it and the token to the server app's exported
 * {@link ExternalClientBroker} in an explicit broadcast through the hidden
 * {@code IActivityManager.broadcastIntentWithFeature} (the {@code am broadcast} path: a raw
 * {@code app_process} is not zygote-forked, so the hidden-API blocklist is not applied to it - a ROM
 * that enforces it there breaks this route; CONTRACT-P11 B1), and waits for the broker's answer.
 * Two connected sockets: it clears their close-on-exec flag and {@code execve}s the program with
 * {@code MOBILEGL_TRANSPORT=spawn}, {@code MOBILEGL_IPC_CONTROL=fd:<control>,<aux>},
 * {@code MOBILEGL_IPC_DATA=shm} and {@code MOBILEGL_IPC_TOKEN} (the session's Hello carries it too).
 * A refusal: it prints the broker's name for it and exits with that refusal's code. No answer at
 * all within the timeout: it says so and exits {@value #EXIT_NO_ANSWER}.
 */
public final class ExternalClientHelper {
    private static final String TAG = "MobileGLHelper";
    private static final String PREFIX = "MobileGL helper: ";

    public static final int EXIT_USAGE = 2;
    public static final int EXIT_BAD_REQUEST = 64;
    public static final int EXIT_TOKEN_MISSING = 65;
    public static final int EXIT_TOKEN_WRONG = 66;
    public static final int EXIT_SERVER_NO_TOKEN = 67;
    public static final int EXIT_SERVER_NOT_RUNNING = 68;
    public static final int EXIT_BUSY = 69;
    public static final int EXIT_CONNECT_FAILED = 70;
    public static final int EXIT_NO_ANSWER = 71;
    public static final int EXIT_BROADCAST_FAILED = 72;
    public static final int EXIT_EXEC_FAILED = 73;

    private static final long DEFAULT_TIMEOUT_MS = 10000;
    private static final Object answered = new Object();
    private static boolean done = false;

    public static void main(String[] args) {
        Looper.prepareMainLooper();
        String serverPackage = BuildConfig.APPLICATION_ID;
        long timeoutMs = DEFAULT_TIMEOUT_MS;
        List<String> program = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            String arg = args[i];
            if (!program.isEmpty()) { program.add(arg); continue; }
            if ("--".equals(arg)) {
                for (int j = i + 1; j < args.length; j++) program.add(args[j]);
                break;
            } else if ("--server".equals(arg) && i + 1 < args.length) {
                serverPackage = args[++i];
            } else if ("--timeout-ms".equals(arg) && i + 1 < args.length) {
                try {
                    timeoutMs = Long.parseLong(args[++i]);
                } catch (NumberFormatException error) {
                    usage("--timeout-ms takes a number of milliseconds");
                }
            } else if ("--token".equals(arg) || arg.startsWith("--token=")) {
                usage("the token is never taken from argv (every process can read it); set MOBILEGL_IPC_TOKEN "
                        + "or MOBILEGL_IPC_TOKEN_FILE in the environment");
            } else if (arg.startsWith("--")) {
                usage("unknown option " + arg);
            } else {
                program.add(arg);
            }
        }
        if (program.isEmpty()) usage("no program to run");

        String token = readToken();
        if (token.isEmpty()) {
            say("no IPC token in MOBILEGL_IPC_TOKEN or MOBILEGL_IPC_TOKEN_FILE; asking the broker anyway, it "
                    + "will refuse by name");
        }
        final String programPath = resolveProgram(program.get(0));
        final List<String> argv = program;
        final String presented = token;
        final String component = serverPackage + "/" + ExternalClientBroker.class.getName();

        // The broker's answer arrives on a binder thread; the execve happens right there, which
        // replaces the whole process, so nothing has to hand the descriptors to the main thread.
        Binder callback = new Binder() {
            @Override
            protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code == ExternalClientBroker.REPLY_REFUSED) {
                    String refusal = data.readString();
                    String message = data.readString();
                    say("refused by the server app (" + component + "): " + refusal + " - " + message);
                    finish(exitCodeFor(refusal));
                    return true;
                }
                if (code != ExternalClientBroker.REPLY_FDS) return false;
                try {
                    int version = data.readInt();
                    ParcelFileDescriptor control = ParcelFileDescriptor.CREATOR.createFromParcel(data);
                    ParcelFileDescriptor aux = ParcelFileDescriptor.CREATOR.createFromParcel(data);
                    int controlFd = inheritable(control);
                    int auxFd = inheritable(aux);
                    Log.i(TAG, "broker v" + version + " handed over fd:" + controlFd + "," + auxFd + "; exec "
                            + programPath);
                    exec(programPath, argv, controlFd, auxFd, presented);
                } catch (Throwable error) {
                    say("could not exec " + programPath + ": " + error);
                    finish(EXIT_EXEC_FAILED);
                }
                return true;
            }
        };

        try {
            sendRequest(serverPackage, presented, callback);
        } catch (Throwable error) {
            say("could not send the request to " + component + " through the hidden IActivityManager: " + error
                    + ". This route needs a raw app_process (not zygote-forked) on a ROM that does not apply the "
                    + "hidden-API blocklist to it (CONTRACT-P11 B1)");
            System.exit(EXIT_BROADCAST_FAILED);
        }

        final long budget = timeoutMs;
        Thread watchdog = new Thread(() -> {
            synchronized (answered) {
                long end = System.currentTimeMillis() + budget;
                while (!done) {
                    long left = end - System.currentTimeMillis();
                    if (left <= 0) break;
                    try {
                        answered.wait(left);
                    } catch (InterruptedException ignored) {
                    }
                }
                if (done) return;
            }
            say("no answer from " + component + " within " + budget + " ms. Is that MobileGL server app "
                    + "installed? If it was not running, the system may have refused to start it for a "
                    + "broadcast (autostart control): open its server screen and start the server first");
            System.exit(EXIT_NO_ANSWER);
        }, "mgl-helper-watchdog");
        watchdog.setDaemon(true);
        watchdog.start();
        Looper.loop();
    }

    private static void finish(int code) {
        synchronized (answered) {
            done = true;
            answered.notifyAll();
        }
        System.exit(code);
    }

    private static int exitCodeFor(String refusal) {
        if (refusal == null) return EXIT_BAD_REQUEST;
        switch (refusal) {
            case ExternalClientBroker.REFUSE_TOKEN_MISSING: return EXIT_TOKEN_MISSING;
            case ExternalClientBroker.REFUSE_TOKEN_WRONG: return EXIT_TOKEN_WRONG;
            case ExternalClientBroker.REFUSE_SERVER_NO_TOKEN: return EXIT_SERVER_NO_TOKEN;
            case ExternalClientBroker.REFUSE_NOT_RUNNING: return EXIT_SERVER_NOT_RUNNING;
            case ExternalClientBroker.REFUSE_BUSY: return EXIT_BUSY;
            case ExternalClientBroker.REFUSE_CONNECT_FAILED: return EXIT_CONNECT_FAILED;
            default: return EXIT_BAD_REQUEST;
        }
    }

    private static void usage(String problem) {
        System.err.println(PREFIX + problem);
        System.err.println("usage: MOBILEGL_IPC_TOKEN=<token> CLASSPATH=<server APK> app_process / "
                + ExternalClientHelper.class.getName()
                + " [--server <package>] [--timeout-ms <n>] [--] <program> [args...]");
        System.exit(EXIT_USAGE);
    }

    private static void say(String line) {
        Log.i(TAG, line);
        System.err.println(PREFIX + line);
    }

    /** MOBILEGL_IPC_TOKEN, else the trimmed content of the file MOBILEGL_IPC_TOKEN_FILE names, else "". */
    private static String readToken() {
        String token = System.getenv("MOBILEGL_IPC_TOKEN");
        if (token != null && !token.isEmpty()) return token;
        String file = System.getenv("MOBILEGL_IPC_TOKEN_FILE");
        if (file == null || file.isEmpty()) return "";
        try {
            return new String(Files.readAllBytes(new File(file).toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception error) {
            say("cannot read MOBILEGL_IPC_TOKEN_FILE=" + file + ": " + error);
            return "";
        }
    }

    /** A bare name is looked up on PATH, as a shell would; anything with a '/' is used as given. */
    private static String resolveProgram(String name) {
        if (name.indexOf('/') >= 0) return name;
        String path = System.getenv("PATH");
        if (path != null) {
            for (String directory : path.split(":")) {
                if (directory.isEmpty()) continue;
                File candidate = new File(directory, name);
                if (candidate.isFile() && candidate.canExecute()) return candidate.getAbsolutePath();
            }
        }
        return name;
    }

    /** Clears FD_CLOEXEC so the descriptor survives execve, and detaches it from its holder. */
    private static int inheritable(ParcelFileDescriptor pfd) throws Exception {
        FileDescriptor fd = pfd.getFileDescriptor();
        int flags = Os.fcntlInt(fd, OsConstants.F_GETFD, 0);
        Os.fcntlInt(fd, OsConstants.F_SETFD, flags & ~OsConstants.FD_CLOEXEC);
        return pfd.detachFd();
    }

    private static void exec(String path, List<String> argv, int control, int aux, String token) throws Exception {
        Map<String, String> env = new LinkedHashMap<>();
        String[] inherited = Os.environ();
        if (inherited != null) {
            for (String entry : inherited) {
                int separator = entry.indexOf('=');
                if (separator > 0) env.put(entry.substring(0, separator), entry.substring(separator + 1));
            }
        }
        // CLASSPATH named the server APK for this helper; the program is not a Java class path user.
        env.remove("CLASSPATH");
        env.put("MOBILEGL_TRANSPORT", "spawn");
        env.put("MOBILEGL_IPC_CONTROL", "fd:" + control + "," + aux);
        env.put("MOBILEGL_IPC_DATA", "shm");
        env.put("MOBILEGL_IPC_TOKEN", token);
        List<String> envp = new ArrayList<>();
        for (Map.Entry<String, String> entry : env.entrySet()) envp.add(entry.getKey() + "=" + entry.getValue());
        Os.execve(path, argv.toArray(new String[0]), envp.toArray(new String[0]));
        throw new IllegalStateException("execve returned");
    }

    /**
     * The explicit broadcast, sent as {@code am broadcast} sends one: {@code ActivityManager.getService()}
     * and its {@code broadcastIntentWithFeature} (or {@code broadcastIntent} on older frameworks) with a
     * null caller. The parameter list differs across framework versions, so it is filled by type: the
     * Intent, false for booleans, null for objects, -1 for ints except the last, which is the user id.
     */
    private static void sendRequest(String serverPackage, String token, IBinder callback) throws Exception {
        Intent intent = new Intent(ExternalClientBroker.ACTION);
        intent.setComponent(new ComponentName(serverPackage, ExternalClientBroker.class.getName()));
        // A stopped app gets the broadcast too, so a server that is not running is refused by name
        // (server-not-running) instead of the request vanishing.
        intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND | Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
        Bundle request = new Bundle();
        request.putBinder(ExternalClientBroker.KEY_CALLBACK, callback);
        request.putString(ExternalClientBroker.KEY_TOKEN, token);
        request.putInt(ExternalClientBroker.KEY_VERSION, ExternalClientBroker.PROTOCOL_VERSION);
        intent.putExtra(ExternalClientBroker.EXTRA_REQUEST, request);

        Object activityManager = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        Method withFeature = null;
        Method plain = null;
        for (Method method : activityManager.getClass().getMethods()) {
            if (method.getName().equals("broadcastIntentWithFeature")) withFeature = method;
            else if (method.getName().equals("broadcastIntent")) plain = method;
        }
        Method broadcast = withFeature != null ? withFeature : plain;
        if (broadcast == null) throw new NoSuchMethodException("IActivityManager has no broadcastIntent*");
        Class<?>[] types = broadcast.getParameterTypes();
        Object[] values = new Object[types.length];
        int lastInt = -1;
        for (int i = 0; i < types.length; i++) if (types[i] == int.class) lastInt = i;
        int userId = Os.getuid() / 100000;
        for (int i = 0; i < types.length; i++) {
            if (types[i] == Intent.class) values[i] = intent;
            else if (types[i] == boolean.class) values[i] = Boolean.FALSE;
            else if (types[i] == int.class) values[i] = i == lastInt ? userId : -1;
            else values[i] = null;
        }
        broadcast.invoke(activityManager, values);
        Log.i(TAG, "request sent to " + serverPackage + " via " + broadcast.getName() + "(" + types.length + " args)");
    }
}
