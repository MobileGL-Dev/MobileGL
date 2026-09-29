package top.mobilegl.plugin;

/**
 * P11 B1 (CONTRACT-P11 B1, ruling ID-P11-12): the two framed {@code PairBind} control messages the
 * broker writes at hand-off - {@code PairBind{nonce, aux=false}} first on the control connection and
 * {@code PairBind{nonce, aux=true}} on the aux one - so the server's PairAcceptor pairs the two
 * connections the moment the broker opens them, not when the program it opened them for reaches its
 * first EGL call.
 *
 * <p>The bytes are the native encoder's own ({@code MG_Remote/Transport/PairBind.h EncodePairBind}
 * framed by {@code Framing.h AppendFrame}): a flatbuffers {@code CtrlEnvelope} whose layout for a
 * 16-byte nonce is fixed, with the nonce as its last 16 bytes. The two prefixes below are that
 * encoder's output up to the nonce, and {@code ServerSpawnTest.TheBrokersPairBindTemplatesAreTheEncoders}
 * reads them out of this file and compares against the encoder for random nonces - a schema change
 * that moves a byte turns that test red, not a device.
 *
 * <p>No android.* import: tools/trace_replay/test_android_lifecycle.py compiles and runs it with javac
 * (PairBindFramesTest).
 */
public final class PairBindFrames {
    private PairBindFrames() {}

    public static final int NONCE_BYTES = 16;
    /** Framing.h kFrameMagic, little-endian on the wire. */
    public static final int FRAME_MAGIC = 0x464C474D;

    // EncodePairBind(nonce, aux=false) up to the nonce (the nonce is the payload's last 16 bytes).
    static final String CONTROL_PREFIX_HEX =
            "100000004d474c4308000e0007000800080000000000000f0c0000000000060008000400060000000400000010000000";
    // EncodePairBind(nonce, aux=true) up to the nonce.
    static final String AUX_PREFIX_HEX =
            "100000004d474c4308000c0007000800080000000000000f0c00000008000c000800070008000000000000010400000010000000";

    /** One framed PairBind: 8-byte header (magic, payload length; little-endian) + the payload. */
    public static byte[] frame(byte[] nonce, boolean aux) {
        if (nonce == null || nonce.length != NONCE_BYTES) {
            throw new IllegalArgumentException("a PairBind nonce is " + NONCE_BYTES + " bytes");
        }
        byte[] prefix = hex(aux ? AUX_PREFIX_HEX : CONTROL_PREFIX_HEX);
        int payload = prefix.length + NONCE_BYTES;
        byte[] out = new byte[8 + payload];
        putLittleEndian(out, 0, FRAME_MAGIC);
        putLittleEndian(out, 4, payload);
        System.arraycopy(prefix, 0, out, 8, prefix.length);
        System.arraycopy(nonce, 0, out, 8 + prefix.length, NONCE_BYTES);
        return out;
    }

    private static void putLittleEndian(byte[] out, int offset, int value) {
        for (int i = 0; i < 4; i++) out[offset + i] = (byte) (value >>> (8 * i));
    }

    static byte[] hex(String text) {
        byte[] out = new byte[text.length() / 2];
        for (int i = 0; i < out.length; i++) out[i] = (byte) Integer.parseInt(text.substring(2 * i, 2 * i + 2), 16);
        return out;
    }
}
