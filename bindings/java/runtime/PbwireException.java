/** A pbwire failure: a pbw_status other than PBW_OK, or a Java-side API misuse. */
public final class PbwireException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    /** The pbw_status behind this failure, or -1 when it has no C counterpart. */
    private final int status;

    public PbwireException(String message) {
        this(message, -1);
    }

    public PbwireException(String message, int status) {
        super(message);
        this.status = status;
    }

    public int status() {
        return status;
    }
}
