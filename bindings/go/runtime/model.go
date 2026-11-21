// Model plumbing for the generated thin models. Marshal/Unmarshal forward
// here, so the generated file holds data only: no capacity hint, no arena, no
// nil handling of its own.
package runtime

import (
	"fmt"
	"reflect"
)

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
	if msg == nil {
		return []byte{}, nil
	}
	if rv := reflect.ValueOf(msg); rv.Kind() == reflect.Ptr && rv.IsNil() {
		return []byte{}, nil
	}
	cap0 := t.capHint()
	if cap0 < 256 {
		cap0 = 256
	}
	e := Encoder{Buf: make([]byte, 0, cap0)}
	if err := encodeTable(&e, msg, t, 0, deterministic); err != nil {
		return nil, err
	}
	t.setCapHint(len(e.Buf) + 64)
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
	if msg == nil {
		return fmt.Errorf("pbwire: unmarshal into a nil message")
	}
	if rv := reflect.ValueOf(msg); rv.Kind() == reflect.Ptr && rv.IsNil() {
		return fmt.Errorf("pbwire: unmarshal into a nil message")
	}
	if err := lim.checkBytes(len(wire)); err != nil {
		return err
	}
	d := Decoder{Buf: wire}
	a := Arena{}
	a.Grow(len(wire))
	return DecodeTableLimits(&d, &a, msg, t, 0, lim)
}
