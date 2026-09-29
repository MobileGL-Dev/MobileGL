package top.mobilegl.plugin;

/**
 * Host-side checks for PairBindFrames, the broker's hand-off PairBinds (P11 B1): the frame header and
 * the nonce's place. That the prefixes ARE the native encoder's bytes is ServerSpawnTest's
 * TheBrokersPairBindTemplatesAreTheEncoders (it reads them out of PairBindFrames.java). Run by
 * tools/trace_replay/test_android_lifecycle.py with plain javac; assert-free like its siblings.
 */
public final class PairBindFramesTest {
    private static int failures = 0;

    private static void expect(String what, Object actual, Object expected) {
        if (expected == null ? actual == null : expected.equals(actual)) return;
        ++failures;
        System.err.println(what + "\n  got:      [" + actual + "]\n  expected: [" + expected + "]");
    }

    private static int littleEndian(byte[] b, int offset) {
        return (b[offset] & 0xff) | (b[offset + 1] & 0xff) << 8 | (b[offset + 2] & 0xff) << 16 | (b[offset + 3] & 0xff) << 24;
    }

    public static void main(String[] args) {
        byte[] nonce = new byte[PairBindFrames.NONCE_BYTES];
        for (int i = 0; i < nonce.length; i++) nonce[i] = (byte) (0xA0 + i);
        for (boolean aux : new boolean[]{false, true}) {
            byte[] frame = PairBindFrames.frame(nonce, aux);
            String role = aux ? "aux" : "control";
            expect(role + ": the frame magic is Framing.h's, little-endian", littleEndian(frame, 0), 0x464C474D);
            expect(role + ": the length field is the payload's", littleEndian(frame, 4), frame.length - 8);
            expect(role + ": the payload is the encoder's size (64 control, 68 aux)", frame.length - 8, aux ? 68 : 64);
            expect(role + ": the envelope carries the MGLC identifier", new String(frame, 12, 4, java.nio.charset.StandardCharsets.US_ASCII), "MGLC");
            byte[] tail = java.util.Arrays.copyOfRange(frame, frame.length - 16, frame.length);
            expect(role + ": the nonce is the last 16 bytes", java.util.Arrays.toString(tail), java.util.Arrays.toString(nonce));
        }
        try {
            PairBindFrames.frame(new byte[8], false);
            ++failures;
            System.err.println("a short nonce was framed");
        } catch (IllegalArgumentException expected) {
        }
        if (failures != 0) {
            System.err.println("PairBindFramesTest: " + failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PairBindFramesTest: all checks passed");
    }
}
