/*
 * pbench candidate: the C core through JNI. Four native methods in
 * jni/pbwire_jni.c, one boundary crossing per operation. Not a JNI binding --
 * nothing here translates a Java tree into a C tree, so a real binding pays
 * strictly more than these numbers.
 */
public final class JniCandidate implements Compare.Candidate {
    static {
        System.loadLibrary("pbwire_jni");
    }

    private static native void nativeInit(byte[] descSet);

    private static native void nativeSetRef(int tier, byte[] wire);

    private static native byte[] nativeEncode(int tier);

    private static native int nativeDecode(byte[] wire);

    private int tier;

    public JniCandidate(byte[] descSet) {
        nativeInit(descSet);
    }

    @Override
    public String name() {
        return "jni-c";
    }

    @Override
    public void prepare(int tier, GenericCodec.DMsg model, byte[] wire) {
        this.tier = tier;
        nativeSetRef(tier, wire);
    }

    @Override
    public byte[] encode() {
        return nativeEncode(tier);
    }

    @Override
    public int decode(byte[] wire) {
        return nativeDecode(wire);
    }
}
