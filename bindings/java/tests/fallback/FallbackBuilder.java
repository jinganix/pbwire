package fallback;

/**
 * Throwing setter behind a non-public type: the {@code Bind} spin cannot
 * link it from another package, so binding falls back to the
 * {@code MethodHandle.invoke} path. The setter body itself is unreachable
 * through the fallback (the invoke-time access check fails first); what
 * matters is the observable contract -- the failure still surfaces as
 * {@code IllegalStateException} with the same "{@code <builder>.<setter>
 * failed}" message.
 */
class FallbackBuilder {
    public void setI32(int v) {
        throw new IllegalArgumentException("boom-fallback");
    }
}
