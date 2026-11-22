// Model plumbing for the generated thin models. Marshal/Unmarshal forward
// here, so the generated file holds data only: no capacity hint, no arena, no
// nil handling of its own.
package runtime

import (
	"fmt"
	"unsafe"
)

// nilMsg reports whether msg is an untyped or typed nil message. Generated
// models always arrive as pointers here, so the interface data word being
// nil means a typed nil pointer; no reflection runs on the hot path.
func nilMsg(msg any) bool {
	if msg == nil {
		return true
	}
	return (*[2]unsafe.Pointer)(unsafe.Pointer(&msg))[1] == nil
}

// EncodeModel serializes a generated model through its table. The capacity
// hint lives on the table, so a steady-state loop reuses the size it already
// paid for without the model carrying any state.
func EncodeModel(msg any, t *Table) ([]byte, error) {
	return encodeModel(msg, t, false)
}

// EncodeModelDeterministic serializes a generated model with map entries in
// key order (see encodeTable): the bytes are stable across runs for equal
// trees, and equal the C deterministic encoder's output. Only map-entry
// rows reorder; every other row keeps its order.
func EncodeModelDeterministic(msg any, t *Table) ([]byte, error) {
	return encodeModel(msg, t, true)
}

func encodeModel(msg any, t *Table, deterministic bool) ([]byte, error) {
	if nilMsg(msg) {
		return []byte{}, nil
	}
	if t == nil {
		return nil, ErrNilTable
	}
	cap0 := t.capHint()
	if cap0 == 0 {
		// Cold table only: one allocation covers the smallest messages.
		// Steady state allocates the learned size, so small payloads stop
		// paying the cold floor on every encode (less zeroing and less GC
		// load under parallel encodes, same single allocation).
		cap0 = 256
	}
	e := Encoder{Buf: make([]byte, 0, cap0)}
	if err := encodeTable(&e, msg, t, 0, deterministic); err != nil {
		return nil, err
	}
	n := len(e.Buf)
	if cap(e.Buf) > cap0 {
		// Overflow: the payload outgrew the hint, so track the new max
		// now instead of reallocating on every call until the size
		// doubles. Overflow writes are rare (one per new max), while
		// steady payloads below stay on the read-only gate in
		// setCapHint. Exact sizing keeps the Go size class minimal.
		t.setCapHintOverflow(n)
	} else {
		t.setCapHint(n)
	}
	return e.Result(), nil
}

// DecodeModel parses wire bytes into a generated model through its table. One
// Arena per call, grown from the input, backs every decoded string and byte
// payload, so the result owns its memory after return.
func DecodeModel(wire []byte, msg any, t *Table) error {
	return DecodeModelLimits(wire, msg, t, Limits{})
}

// DecodeModelLimits is DecodeModel with explicit bounds: inputs larger than
// the byte cap fail before anything decodes, and nesting past the effective
// cap fails. The old name keeps its signature as a defaults wrapper.
func DecodeModelLimits(wire []byte, msg any, t *Table, lim Limits) error {
	if nilMsg(msg) {
		return fmt.Errorf("pbwire: unmarshal into a nil message")
	}
	if t == nil {
		return ErrNilTable
	}
	if err := lim.checkBytes(len(wire)); err != nil {
		return err
	}
	d := Decoder{Buf: wire}
	a := Arena{}
	// Steady decodes presize from what the last one stored, not from the
	// input length: string/bytes payload is usually a fraction of the
	// wire, so sizing from the input over-allocates (more zeroing, more GC
	// load) on every call. Overflows relearn immediately, steadier sizes
	// through the gate (see setArenaHint). The read is clamped to the
	// input length: a stale bulk hint must not Grow kilobytes for a
	// hundred-byte payload on a size switch (a gateway alternating sizes
	// would pay it on every switch); the clamp keeps every transition
	// exact while steady sizes reuse their converged hint.
	initCap := 0
	if h := t.arenaHint(); h > 0 && h < len(wire) {
		a.Grow(h)
		initCap = cap(a.buf)
	} else {
		a.Grow(len(wire))
	}
	if err := DecodeTableLimits(&d, &a, msg, t, 0, lim); err != nil {
		return err
	}
	if initCap > 0 && cap(a.buf) > initCap {
		t.setArenaHintOverflow(len(a.buf))
	} else {
		t.setArenaHint(len(a.buf))
	}
	return nil
}
