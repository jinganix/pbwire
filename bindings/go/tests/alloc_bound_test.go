package tests

import (
	"math"
	"testing"

	"google.golang.org/protobuf/proto"
	"pbwire/tests/gen/matrix3"
	pb1 "pbwire/tests/gen_oracle/matrix3pb"
)

// Medium-tier thin-model decode stays at 10 allocations: packed runs must
// not pick up per-run overhead behind the shared engine (a past presize
// attempt pushed appendFastPacked over the inline budget, escaping the run
// cursor at +1 alloc per packed run: 10 -> 12 with identical bytes). The
// bound has one count of headroom for in-process background activity; the
// oracle takes 14 on the same payload.
func TestMediumDecodeAllocBound(t *testing.T) {
	official := &pb1.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64,
		U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64,
		Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64,
		F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: pb1.Color3_COLOR3_GREEN,
		Inner:  &pb1.Inner3{Name: "kid", Value: 42},
		OptI32: func() *int32 { v := int32(-7); return &v }(),
		OptStr: func() *string { v := "opt"; return &v }(),
		Nums:   []int32{-3},
		Tags:   []string{"tag"},
		Inners: []*pb1.Inner3{{Name: "grandkid", Value: 1}},
		Flags:  []bool{true},
	}
	reference, err := proto.Marshal(official)
	if err != nil {
		t.Fatal(err)
	}
	if len(reference) != 174 {
		t.Fatalf("medium fixture is %d bytes, want 174", len(reference))
	}
	msg := &matrix3.Scalars3{}
	if err := msg.Unmarshal(reference); err != nil {
		t.Fatal(err)
	}
	got := testing.AllocsPerRun(10, func() {
		m := new(matrix3.Scalars3)
		_ = m.Unmarshal(reference)
	})
	if got > 11 {
		t.Errorf("medium decode allocs = %.0f, want <= 11", got)
	}
}
