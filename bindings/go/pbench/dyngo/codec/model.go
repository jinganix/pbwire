// Model plumbing for the generated thin models. Marshal/Unmarshal forward
// here, so the generated file holds data only: no capacity hint, no arena, no
// nil handling of its own.
package codec

import (
	"fmt"
	"reflect"
)

// EncodeModel serializes a generated model through its table. The capacity
// hint lives on the table, so a steady-state loop reuses the size it already
// paid for without the model carrying any state.
func EncodeModel(msg any, t *Table) ([]byte, error) {
	if msg == nil || (reflect.ValueOf(msg).Kind() == reflect.Ptr && reflect.ValueOf(msg).IsNil()) {
		return []byte{}, nil
	}
	cap0 := t.capHint()
	if cap0 < 256 {
		cap0 = 256
	}
	e := Encoder{Buf: make([]byte, 0, cap0)}
	if err := EncodeTable(&e, msg, t, 0); err != nil {
		return nil, err
	}
	t.setCapHint(len(e.Buf) + 64)
	return e.Result(), nil
}

// DecodeModel parses wire bytes into a generated model through its table. One
// Arena per call, grown from the input, backs every decoded string and byte
// payload, so the result owns its memory after return.
func DecodeModel(wire []byte, msg any, t *Table) error {
	if msg == nil || (reflect.ValueOf(msg).Kind() == reflect.Ptr && reflect.ValueOf(msg).IsNil()) {
		return fmt.Errorf("pbwire: unmarshal into a nil message")
	}
	d := Decoder{Buf: wire}
	a := Arena{}
	a.Grow(len(wire))
	return DecodeTable(&d, &a, msg, t, 0)
}
