package top.mobilegl.plugin;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * P11 B1 (CONTRACT-P11 B1): the connection broker for a same-device client in ANOTHER security
 * context - a native GL program started from Termux, adb shell or another app.
 *
 * <p>SELinux refuses such a client the {@code connect()} to this app's unix endpoint
 * ({@code connectto}, B0-CROSS-APP.md), but this app may connect to its own. So the client's
 * {@link ExternalClientHelper} (an {@code app_process} under the client's uid) sends this exported
 * receiver an explicit broadcast carrying a callback {@link IBinder} and the IPC token; this
 * receiver checks the token against the running server's own ({@code MOBILEGL_IPC_TOKEN}), connects
 * TWICE to the server's unix endpoint - or, when the server listens on TCP only, to the private
 * abstract listener {@link MobileGLServerService} runs beside it for this - and hands both client
 * ends back as {@link ParcelFileDescriptor}s. It writes nothing on them: the client presents its own
 * PairBind pair ({@code MOBILEGL_IPC_CONTROL=fd:<control>,<aux>}).
 *
 * <p>EVERY request is answered on the callback - the two descriptors, or a refusal with its name -
 * so a helper never has to time out to learn it was refused. The refusals: {@code bad-request},
 * {@code server-not-running}, {@code server-no-token}, {@code token-missing}, {@code token-wrong},
 * {@code broker-busy}, {@code connect-failed}.
 *
 * <p>Exported in BOTH flavours: an exported component has no category wall in front of it, so the
 * token IS the guard. A server started without a token of at least {@value #MIN_TOKEN_BYTES} bytes
 * hands out nothing. Runs in the server's own process ({@code :mglsrv}), where the running
 * service's state is; a broadcast that finds no service there ({@code server-not-running}) cannot
 * start one - Android 12+ forbids starting a foreground service from a background receiver.
 */
public final class ExternalClientBroker extends BroadcastReceiver {
    private static final String TAG = "MobileGLBroker";

    public static final String ACTION = "top.mobilegl.plugin.EXTERNAL_CLIENT_CONNECT";
    public static final String EXTRA_REQUEST = "mobilegl_broker";
    public static final String KEY_CALLBACK = "callback";
    public static final String KEY_TOKEN = "token";
    public static final String KEY_VERSION = "version";
    public static final int PROTOCOL_VERSION = 1;

    /** The broker's answer: int version, then the control and aux descriptors. */
    public static final int REPLY_FDS = IBinder.FIRST_CALL_TRANSACTION;
    /** The broker's refusal: String code, String message. */
    public static final int REPLY_REFUSED = IBinder.FIRST_CALL_TRANSACTION + 1;

    public static final String REFUSE_BAD_REQUEST = "bad-request";
    public static final String REFUSE_NOT_RUNNING = "server-not-running";
    public static final String REFUSE_SERVER_NO_TOKEN = "server-no-token";
    public static final String REFUSE_TOKEN_MISSING = "token-missing";
    public static final String REFUSE_TOKEN_WRONG = "token-wrong";
    public static final String REFUSE_BUSY = "broker-busy";
    public static final String REFUSE_CONNECT_FAILED = "connect-failed";

    /** The same floor as a non-loopback TCP listen (MG_Remote/Transport/AuthToken.h). */
    public static final int MIN_TOKEN_BYTES = 16;
    /** How long the two connects may take before the listener is called busy. */
    private static final long CONNECT_BUDGET_MS = 3000;

    // One brokering at a time: a second request while one is connecting is refused by name.
    private static final AtomicBoolean inFlight = new AtomicBoolean(false);

    @Override
    public void onReceive(Context context, Intent intent) {
        Bundle request = intent == null ? null : intent.getBundleExtra(EXTRA_REQUEST);
        IBinder callback = request == null ? null : request.getBinder(KEY_CALLBACK);
        if (callback == null) {
            // Nobody to answer: the one refusal that can only be logged.
            Log.e(TAG, "refused by name: bad-request - no " + EXTRA_REQUEST + " bundle with a callback binder");
            return;
        }
        int version = request.getInt(KEY_VERSION, 0);
        if (version != PROTOCOL_VERSION) {
            refuse(callback, REFUSE_BAD_REQUEST, "broker protocol version " + version + " is not "
                    + PROTOCOL_VERSION + "; use the helper from this server's own APK (the launch command on "
                    + "its server screen)");
            return;
        }
        MobileGLServerService.BrokerTarget target = MobileGLServerService.brokerTarget();
        if (target == null) {
            refuse(callback, REFUSE_NOT_RUNNING, "the MobileGL render server is not running. Start it on the "
                    + "MobileGL app's server screen first (offscreen mode); the broker cannot start it - "
                    + "Android 12+ forbids starting a foreground service from a background receiver");
            return;
        }
        if (target.token == null || target.token.getBytes(StandardCharsets.UTF_8).length < MIN_TOKEN_BYTES) {
            refuse(callback, REFUSE_SERVER_NO_TOKEN, "the render server was started without an IPC token of at "
                    + "least " + MIN_TOKEN_BYTES + " bytes, so it takes no same-device external clients. Set the "
                    + "Auth token on its server screen and restart it");
            return;
        }
        String presented = request.getString(KEY_TOKEN);
        if (presented == null || presented.isEmpty()) {
            refuse(callback, REFUSE_TOKEN_MISSING, "no IPC token presented; set MOBILEGL_IPC_TOKEN (or "
                    + "MOBILEGL_IPC_TOKEN_FILE) in the helper's environment");
            return;
        }
        if (!constantTimeEquals(target.token, presented)) {
            refuse(callback, REFUSE_TOKEN_WRONG, "the presented IPC token is not this server's; handing out no "
                    + "descriptors");
            return;
        }
        if (!inFlight.compareAndSet(false, true)) {
            refuse(callback, REFUSE_BUSY, "another client's connection is being brokered right now; retry");
            return;
        }
        // The connects run off the main thread and onReceive returns at once, so the next request
        // is delivered (and refused busy) rather than queued behind this one. The process is the
        // server's foreground-service process, so it outlives the broadcast.
        new Thread(() -> {
            try {
                broker(callback, target);
            } finally {
                inFlight.set(false);
            }
        }, "mgl-broker").start();
    }

    private static void broker(IBinder callback, MobileGLServerService.BrokerTarget target) {
        final LocalSocket[] sockets = new LocalSocket[2];
        final Exception[] failure = new Exception[1];
        Thread connector = new Thread(() -> {
            try {
                sockets[0] = connect(target.endpoint);
                sockets[1] = connect(target.endpoint);
            } catch (Exception error) {
                failure[0] = error;
            }
        }, "mgl-broker-connect");
        connector.start();
        try {
            connector.join(CONNECT_BUDGET_MS);
        } catch (InterruptedException ignored) {
        }
        if (connector.isAlive()) {
            // A full listen backlog (or a server that stopped accepting) blocks a unix connect.
            closeQuietly(sockets[0]);
            closeQuietly(sockets[1]);
            refuse(callback, REFUSE_BUSY, "the server's listener " + target.endpoint + " did not accept two "
                    + "connections within " + CONNECT_BUDGET_MS + " ms; retry");
            return;
        }
        if (failure[0] != null || sockets[0] == null || sockets[1] == null) {
            closeQuietly(sockets[0]);
            closeQuietly(sockets[1]);
            refuse(callback, REFUSE_CONNECT_FAILED, "could not connect to the server's endpoint " + target.endpoint
                    + ": " + failure[0]);
            return;
        }
        ParcelFileDescriptor control = null;
        ParcelFileDescriptor aux = null;
        Parcel data = Parcel.obtain();
        try {
            control = ParcelFileDescriptor.dup(sockets[0].getFileDescriptor());
            aux = ParcelFileDescriptor.dup(sockets[1].getFileDescriptor());
            data.writeInt(PROTOCOL_VERSION);
            control.writeToParcel(data, 0);
            aux.writeToParcel(data, 0);
            boolean delivered = callback.transact(REPLY_FDS, data, null, IBinder.FLAG_ONEWAY);
            Log.i(TAG, "brokered a connection pair to " + target.endpoint + " for a client (delivered="
                    + delivered + ")");
        } catch (Exception error) {
            Log.e(TAG, "could not hand the connection pair to the client", error);
        } finally {
            data.recycle();
            // Our copies go; the client's (received over Binder) are its own descriptors.
            closeQuietly(control);
            closeQuietly(aux);
            closeQuietly(sockets[0]);
            closeQuietly(sockets[1]);
        }
    }

    private static LocalSocket connect(String endpoint) throws Exception {
        boolean abstractName = endpoint.startsWith("@");
        LocalSocket socket = new LocalSocket();
        try {
            socket.connect(new LocalSocketAddress(abstractName ? endpoint.substring(1) : endpoint,
                    abstractName ? LocalSocketAddress.Namespace.ABSTRACT : LocalSocketAddress.Namespace.FILESYSTEM));
        } catch (Exception error) {
            closeQuietly(socket);
            throw error;
        }
        return socket;
    }

    private static void refuse(IBinder callback, String code, String message) {
        Log.w(TAG, "refused by name: " + code + " - " + message);
        Parcel data = Parcel.obtain();
        try {
            data.writeString(code);
            data.writeString(message);
            callback.transact(REPLY_REFUSED, data, null, IBinder.FLAG_ONEWAY);
        } catch (Exception error) {
            Log.w(TAG, "the client's callback is gone; the refusal was only logged", error);
        } finally {
            data.recycle();
        }
    }

    static boolean constantTimeEquals(String expected, String presented) {
        byte[] x = expected.getBytes(StandardCharsets.UTF_8);
        byte[] y = presented.getBytes(StandardCharsets.UTF_8);
        int difference = x.length ^ y.length;
        for (int i = 0; i < x.length; i++) difference |= x[i] ^ (i < y.length ? y[i] : 0);
        return difference == 0;
    }

    private static void closeQuietly(AutoCloseable closeable) {
        if (closeable == null) return;
        try {
            closeable.close();
        } catch (Exception ignored) {
        }
    }
}
