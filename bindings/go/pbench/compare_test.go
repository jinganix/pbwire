//go:build pbench

// Interleaved comparison of the two dynamic arms on the same payload:
// dync is the C core over cgo and dyngo is the Go core (value tree, no cgo). Every arm does
// the same user-visible task, native struct in and native struct out, and every
// impl is measured in the same round, back to back, so machine load hits all of
// them equally and the median across rounds is the number to read.
//
//	go test -tags pbench -run TestCompare -v ./pbench/
package pbench

import (
	"fmt"
	"math"
	"sort"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
	"pbwire/pbench/dync"
	"pbwire/pbench/dyngo"
	"pbwire/tests/gen/matrix3"
	"pbwire/tests/gen_oracle/matrix3pb"
)

func i32p(v int32) *int32   { return &v }
func strp(v string) *string { return &v }

func fill(n int) *matrix3pb.Scalars3 {
	m := &matrix3pb.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64, U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64, Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: matrix3pb.Color3_COLOR3_GREEN,
		Inner:  &matrix3pb.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7), OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3pb.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

var tiers = []struct {
	label   string
	n, want int
	iters   int
}{
	{"136B", 0, 136, 20000},
	{"174B", 1, 174, 10000},
	{"2469B", 100, 2469, 1000},
	{"74913B", 3000, 74913, 200},
}

// codec is what every variant satisfies; adapters below keep V0 in the same
// shape so one loop times all of them.
type codec interface {
	Name() string
	Marshal(m *matrix3.Scalars3) ([]byte, error)
	Unmarshal(wire []byte) (*matrix3.Scalars3, error)
}

// checkStruct compares a decoded struct against the protobuf-go decode of the
// same bytes, field by field.
func checkStruct(label string, got *matrix3.Scalars3, want *matrix3pb.Scalars3) error {
	if got.I32 != want.I32 || got.I64 != want.I64 || got.U32 != want.U32 || got.U64 != want.U64 ||
		got.S32 != want.S32 || got.S64 != want.S64 || got.Fx32 != want.Fx32 || got.Fx64 != want.Fx64 ||
		got.Sfx32 != want.Sfx32 || got.Sfx64 != want.Sfx64 || got.F32 != want.F32 || got.F64 != want.F64 ||
		got.Flag != want.Flag || got.Str != want.Str || int32(got.Color) != int32(want.Color) {
		return fmt.Errorf("%s: scalar mismatch", label)
	}
	if got.Inner.GetName() != want.Inner.GetName() || got.Inner.GetValue() != want.Inner.GetValue() {
		return fmt.Errorf("%s: inner mismatch", label)
	}
	if (got.OptI32 == nil) != (want.OptI32 == nil) || (got.OptI32 != nil && *got.OptI32 != *want.OptI32) {
		return fmt.Errorf("%s: opt_i32 mismatch", label)
	}
	if (got.OptStr == nil) != (want.OptStr == nil) || (got.OptStr != nil && *got.OptStr != *want.OptStr) {
		return fmt.Errorf("%s: opt_str mismatch", label)
	}
	if len(got.Nums) != len(want.Nums) || len(got.Tags) != len(want.Tags) ||
		len(got.Flags) != len(want.Flags) || len(got.Inners) != len(want.Inners) {
		return fmt.Errorf("%s: repeated length mismatch", label)
	}
	for i := range want.Nums {
		if got.Nums[i] != want.Nums[i] || got.Tags[i] != want.Tags[i] ||
			got.Flags[i] != want.Flags[i] || got.Inners[i].Name != want.Inners[i].Name ||
			got.Inners[i].Value != want.Inners[i].Value {
			return fmt.Errorf("%s: repeated element %d mismatch", label, i)
		}
	}
	return nil
}

func TestCompare(t *testing.T) {
	cDync, err := dync.New()
	if err != nil {
		t.Fatal(err)
	}
	defer cDync.Close()
	cDynGo, err := dyngo.New()
	if err != nil {
		t.Fatal(err)
	}
	impls := []codec{cDync, cDynGo}
	names := []string{"dync (C core)", "dyngo (Go core)", "protobuf-go"}

	const rounds = 5

	for _, tr := range tiers {
		want := fill(tr.n)
		ref, err := proto.Marshal(want)
		if err != nil {
			t.Fatal(err)
		}
		if len(ref) != tr.want {
			t.Fatalf("%s: official wrote %d bytes, want %d", tr.label, len(ref), tr.want)
		}

		// Gate: same bytes out, same fields in, for every arm.
		for _, impl := range impls {
			back, err := impl.Unmarshal(ref)
			if err != nil {
				t.Fatalf("%s %s: %v", tr.label, impl.Name(), err)
			}
			if err := checkStruct(tr.label+" "+impl.Name(), back, want); err != nil {
				t.Fatal(err)
			}
			gob, err := impl.Marshal(back)
			if err != nil || string(gob) != string(ref) {
				t.Fatalf("%s %s: encode differs from protobuf-go", tr.label, impl.Name())
			}
		}
		base := &matrix3.Scalars3{}
		if err := base.Unmarshal(ref); err != nil {
			t.Fatal(err)
		}

		enc := make([][]float64, len(impls)+1)
		dec := make([][]float64, len(impls)+1)
		for r := 0; r < rounds; r++ {
			// Encode: every impl from the same prebuilt native struct.
			for i, impl := range impls {
				enc[i] = append(enc[i], runN(tr.iters, func() {
					sink, _ = impl.Marshal(base)
				}))
			}
			enc[len(impls)] = append(enc[len(impls)], runN(tr.iters, func() {
				sink, _ = proto.Marshal(want)
			}))
			// Decode: every impl into a fresh native struct.
			for i, impl := range impls {
				dec[i] = append(dec[i], runN(tr.iters, func() {
					sinkMsg, _ = impl.Unmarshal(ref)
				}))
			}
			dec[len(impls)] = append(dec[len(impls)], runN(tr.iters, func() {
				var m matrix3pb.Scalars3
				_ = proto.Unmarshal(ref, &m)
			}))
		}

		fmt.Printf("\n%s (%d bytes, %d iterations, median of %d interleaved rounds)\n", tr.label, len(ref), tr.iters, rounds)
		fmt.Printf("  %-22s %10s %10s %8s   %10s %8s\n", "impl", "enc ns/op", "x pb-go", "allocs", "dec ns/op", "x pb-go")
		for i := 0; i < len(impls); i++ {
			fmt.Printf("  %-22s %10.1f %10.2f %8.0f   %10.1f %10.2f\n",
				names[i], median(enc[i]), median(enc[len(impls)])/median(enc[i]),
				allocs(func() { sink, _ = impls[i].Marshal(base) }),
				median(dec[i]), median(dec[len(impls)])/median(dec[i]))
		}
		fmt.Printf("  %-22s %10.1f %10s %8.0f   %10.1f %10s\n", names[len(impls)],
			median(enc[len(impls)]), "-", allocs(func() { sink, _ = proto.Marshal(want) }),
			median(dec[len(impls)]), "-")
	}
}

func runN(n int, op func()) float64 {
	start := time.Now()
	for i := 0; i < n; i++ {
		op()
	}
	return float64(time.Since(start).Nanoseconds()) / float64(n)
}

func median(v []float64) float64 {
	s := append([]float64(nil), v...)
	sort.Float64s(s)
	return s[len(s)/2]
}

func allocs(op func()) float64 { return testing.AllocsPerRun(3, op) }

var (
	sink    []byte
	sinkMsg *matrix3.Scalars3
)
