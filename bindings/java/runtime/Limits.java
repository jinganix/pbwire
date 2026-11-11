/**
 * Bounds for untrusted input, like the core's {@code pbw_limits}. Zero
 * fields mean the defaults: no byte cap and {@link Wire#MAX_DEPTH} nesting.
 * A max depth above the cap clamps to it.
 */
public final class Limits {
    /** Rejects inputs larger than this; 0 means no cap. */
    public final long maxBytes;
    /** Nesting cap; 0 means {@link Wire#MAX_DEPTH}. */
    public final int maxDepth;

    public Limits(long maxBytes, int maxDepth) {
        if (maxBytes < 0 || maxDepth < 0) {
            throw new PbwireException("limits must not be negative");
        }
        this.maxBytes = maxBytes;
        this.maxDepth = maxDepth;
    }

    /** No byte cap, {@link Wire#MAX_DEPTH} nesting. */
    public static Limits defaults() {
        return new Limits(0, 0);
    }

    /** The depth cap this limit enforces on a decode. */
    int effectiveDepth() {
        if (maxDepth == 0 || maxDepth > Wire.MAX_DEPTH) {
            return Wire.MAX_DEPTH;
        }
        return maxDepth;
    }

    /** Fails an input larger than the byte cap. */
    void checkBytes(long n, String what) {
        if (maxBytes > 0 && n > maxBytes) {
            throw new PbwireException(what + " exceeds the "
                    + maxBytes + "-byte limit", 5);
        }
    }
}
