// Thin model plumbing: the generated file forwards to runtime.EncodeModel/
// DecodeModel and carries no encoder state of its own.
package runtime_test

import (
	"testing"

	"pbwire/runtime"
	"pbwire/tests/gen/matrix3"
)

func TestThinModelEncoding(t *testing.T) {
	src := fillScalarsStruct(2)
	wire, err := runtime.EncodeModel(src, matrix3.Scalars3Meta)
	if err != nil {
		t.Fatal(err)
	}
	got, err := src.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(wire) {
		t.Fatalf("Marshal % x != EncodeModel % x", got, wire)
	}
	var back matrix3.Scalars3
	if err := runtime.DecodeModel(wire, &back, matrix3.Scalars3Meta); err != nil {
		t.Fatal(err)
	}
	if back.Str != "hello" || len(back.Nums) != 2 || back.Inner.GetName() != "kid" {
		t.Fatalf("decode lost fields: %+v", back)
	}
	var nilMsg *matrix3.Scalars3
	if _, err := nilMsg.Marshal(); err != nil {
		t.Fatalf("nil Marshal: %v", err)
	}
	if err := nilMsg.Unmarshal(wire); err == nil {
		t.Fatal("Unmarshal into nil must fail")
	}
}

// A nil table is a caller error, not a crash: every model entry point
// rejects it like a nil message, even though generated code never
// passes one (e.g. after an ignored Type error).
func TestNilTableReturnsError(t *testing.T) {
	src := fillScalarsStruct(1)
	if _, err := runtime.EncodeModel(src, nil); err == nil {
		t.Fatal("EncodeModel with nil table must fail")
	}
	var back matrix3.Scalars3
	if err := runtime.DecodeModel([]byte{0x08, 0x01}, &back, nil); err == nil {
		t.Fatal("DecodeModel with nil table must fail")
	}
	d := runtime.Decoder{Buf: []byte{0x08, 0x01}}
	a := runtime.Arena{}
	if err := runtime.DecodeTable(&d, &a, &back, nil, 0); err == nil {
		t.Fatal("DecodeTable with nil table must fail")
	}
}
