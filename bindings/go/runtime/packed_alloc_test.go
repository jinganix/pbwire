package runtime_test

// TDD gate for the parallel-decode gap (issue: decode-par ~7% behind
// protobuf-go on large/xlarge tiers): packed runs must not ride geometric
// growth nor allocate a parse closure per run. Both costs scale with
// element count, so the 100/3000-element tiers pay them while small tiers
// do not, which is exactly the observed shape.
import (
	"testing"

	"pbwire/tests/gen/matrix3"
)

func largePackedFixture() *matrix3.Scalars3 {
	m := &matrix3.Scalars3{Str: "hello", Inner: &matrix3.Inner3{Name: "kid", Value: 42}}
	for i := 0; i < 100; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

// TestPackedDecodeBoundedAllocs pins the allocation budget of a 100-element
// packed decode: 100 Inner structs + slice growth + arena + optionals is
// the floor; geometric growth on the two packed runs (Nums/Flags) plus one
// escaping closure per run puts us at 131. Presized runs land at most 120.
func TestPackedDecodeBoundedAllocs(t *testing.T) {
	wire, err := largePackedFixture().Marshal()
	if err != nil {
		t.Fatal(err)
	}
	a := testing.AllocsPerRun(10, func() {
		n := new(matrix3.Scalars3)
		if err := n.Unmarshal(wire); err != nil {
			t.Fatal(err)
		}
	})
	t.Logf("packed decode allocs/op=%v wire=%d", a, len(wire))
	if a > 120 {
		t.Fatalf("packed decode allocs/op=%v, want <=120 (presized packed runs, no per-run closure)", a)
	}
}
