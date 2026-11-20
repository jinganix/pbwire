// Thin model plumbing: the generated file forwards to codec.EncodeModel/
// DecodeModel and carries no encoder state of its own.
package codec_test

import (
	"testing"

	"pbwire/codec"
	"pbwire/tests/gen/matrix3"
)

func TestThinModelEncoding(t *testing.T) {
	src := fillScalarsStruct(2)
	wire, err := codec.EncodeModel(src, matrix3.Scalars3Meta)
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
	if err := codec.DecodeModel(wire, &back, matrix3.Scalars3Meta); err != nil {
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
