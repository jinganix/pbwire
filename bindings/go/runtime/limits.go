// Decode bounds for untrusted input. Mirrors pbw_limits in include/pbwire.h:
// zero fields mean the defaults (no byte cap, MaxDepth nesting); a depth
// cap above the hard maximum clamps to it, like pbw_effective_depth.
package runtime

import "errors"

// ErrLimit reports input past Limits.MaxBytes: a top-level buffer, a JSON
// or text document, or a delimited frame declared longer than the cap. The
// check runs before anything decodes, so a breach fails at offset 0 and
// consumes nothing. Nesting past the cap stays ErrDepth, the sentinel the
// depth check has always returned.
var ErrLimit = errors.New("codec: input exceeds size limit")

// Limits bounds one decode call. It is passed by value and read once per
// message, so the unlimited path pays no per-field branch for a mode it
// never uses.
type Limits struct {
	// MaxBytes rejects inputs larger than this; 0 means no cap.
	MaxBytes int
	// MaxDepth caps message/group nesting; 0 or above MaxDepth means
	// MaxDepth.
	MaxDepth int
}

// effectiveDepth resolves the nesting cap: 0 or an over-large value means
// the hard default, like the C core's clamp.
func (l Limits) effectiveDepth() int {
	if l.MaxDepth <= 0 || l.MaxDepth > MaxDepth {
		return MaxDepth
	}
	return l.MaxDepth
}

// checkBytes rejects a top-level input larger than the cap.
func (l Limits) checkBytes(n int) error {
	if l.MaxBytes > 0 && n > l.MaxBytes {
		return ErrLimit
	}
	return nil
}

// checkDepth rejects nesting past the effective cap. One compare per
// message, like CheckDepth before it.
func (l Limits) checkDepth(depth int) error {
	if depth > l.effectiveDepth() {
		return ErrDepth
	}
	return nil
}
