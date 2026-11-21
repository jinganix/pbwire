package tests

// Regression: implicit proto3 float/double holding negative zero. The
// reference serializer emits -0.0 (its bits are nonzero); the thin-model
// fast path used to drop it with a plain == 0 check, diverging from the
// closure path and the oracle. Positive zero must still be omitted, so
// both directions are pinned here against protobuf-go byte for byte.

import (
	"bytes"
	"math"
	"testing"

	"google.golang.org/protobuf/proto"

	gen1 "pbwire/tests/gen/matrix3"
	pb1 "pbwire/tests/gen_oracle/matrix3pb"
)

func TestNegativeZeroFloat(t *testing.T) {
	n32 := float32(math.Copysign(0, -1))
	n64 := math.Copysign(0, -1)
	if math.Float32bits(n32) == 0 || math.Float64bits(n64) == 0 {
		t.Fatal("test setup: not negative zero")
	}
	oracle := &pb1.Scalars3{F32: n32, F64: n64}
	mine := &gen1.Scalars3{F32: n32, F64: n64}
	ow, err := proto.Marshal(oracle)
	if err != nil {
		t.Fatal(err)
	}
	fw, err := mine.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	// 11 fixed32 + 12 fixed64, both present: (1+4) + (1+8).
	if len(ow) != 14 {
		t.Fatalf("oracle did not emit -0.0: %d bytes %x", len(ow), ow)
	}
	if !bytes.Equal(fw, ow) {
		t.Fatalf("thin model diverges on -0.0: %x vs %x", fw, ow)
	}
	back := &gen1.Scalars3{}
	if err := back.Unmarshal(fw); err != nil {
		t.Fatal(err)
	}
	if math.Float32bits(back.GetF32()) != math.Float32bits(n32) ||
		math.Float64bits(back.GetF64()) != math.Float64bits(n64) {
		t.Fatalf("round trip lost -0.0: %+v", back)
	}

	// Positive zero stays omitted on both sides.
	emptyOracle, err := proto.Marshal(&pb1.Scalars3{})
	if err != nil {
		t.Fatal(err)
	}
	emptyMine, err := (&gen1.Scalars3{}).Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if len(emptyOracle) != 0 || len(emptyMine) != 0 {
		t.Fatalf("zero value must marshal empty: %d vs %d", len(emptyOracle), len(emptyMine))
	}

	// Infinities still emit identically.
	infOracle, err := proto.Marshal(&pb1.Scalars3{
		F32: float32(math.Inf(1)),
		F64: math.Inf(-1),
	})
	if err != nil {
		t.Fatal(err)
	}
	infMine, err := (&gen1.Scalars3{
		F32: float32(math.Inf(1)),
		F64: math.Inf(-1),
	}).Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(infMine, infOracle) {
		t.Fatalf("inf diverges: %x vs %x", infMine, infOracle)
	}
}
