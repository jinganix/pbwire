// Benchmark the generated Go codec against the official protobuf runtime
// (google.golang.org/protobuf generated code) and vtproto (the fastest
// widely used Go generator) on the same matrix3.proto and the four standard
// payload tiers from BENCHMARKS.md. vtproto's methods attach to the official
// types (its generated file lands in gen/official), so all three encoders
// share one fixture and one byte-parity gate.
//
// All sides encode the same values and the harness refuses to time anything
// until every encoder produces identical bytes; decode is timed on a fresh
// message per iteration, like the C and Python harnesses.
//
//	cd bindings/go && go run ./benchmarks        # generate the sources first:
//	make go-bench                                # from the repo root
package main

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"testing"

	"google.golang.org/protobuf/proto"

	official "pbwire/benchmarks/gen/official"
	pbwire "pbwire/benchmarks/gen/pbwire"
)

const desc = "matrix3.proto"

var tiers = []struct {
	label   string
	repeats int
}{
	{"small", 0},
	{"medium", 1},
	{"large", 100},
	{"xlarge", 3000},
}

type row struct {
	impl string
	op   string
	ns   float64
	mbps float64
}

func main() {
	fmt.Printf("gen: protoc-gen-pbwire-go (generated) vs google.golang.org/protobuf (official) vs vtproto (fastest)\n")
	fmt.Printf("message: structtest.Scalars3, source %s\n\n", desc)
	failures := 0
	for _, tier := range tiers {
		wire := verify(tier.repeats)
		goMsg := fillPB(tier.repeats)
		officialMsg := fillOfficial(tier.repeats)
		encodeGo, n := measure("go", "encode", len(wire), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				if _, err := goMsg.Marshal(); err != nil {
					b.Fatal(err)
				}
			}
		})
		fmt.Printf("%s: %d bytes, %d iterations (repeat field x%d)\n",
			tier.label, len(wire), n, tier.repeats)
		fmt.Printf("  %-9s %-7s %12s %10s\n", "impl", "op", "ns/op", "MB/s")
		rows := []row{
			encodeGo,
			mustMeasure("official", "encode", len(wire), func(b *testing.B) {
				for i := 0; i < b.N; i++ {
					if _, err := proto.Marshal(officialMsg); err != nil {
						b.Fatal(err)
					}
				}
			}),
			mustMeasure("vtproto", "encode", len(wire), func(b *testing.B) {
				for i := 0; i < b.N; i++ {
					if _, err := officialMsg.MarshalVT(); err != nil {
						b.Fatal(err)
					}
				}
			}),
			mustMeasure("go", "decode", len(wire), func(b *testing.B) {
				for i := 0; i < b.N; i++ {
					var out pbwire.Scalars3
					if err := out.Unmarshal(wire); err != nil {
						b.Fatal(err)
					}
				}
			}),
			mustMeasure("official", "decode", len(wire), func(b *testing.B) {
				for i := 0; i < b.N; i++ {
					var out official.Scalars3
					if err := proto.Unmarshal(wire, &out); err != nil {
						b.Fatal(err)
					}
				}
			}),
			mustMeasure("vtproto", "decode", len(wire), func(b *testing.B) {
				for i := 0; i < b.N; i++ {
					var out official.Scalars3
					if err := out.UnmarshalVT(wire); err != nil {
						b.Fatal(err)
					}
				}
			}),
		}
		for _, r := range rows {
			fmt.Printf("  %-9s %-7s %12.1f %10.1f\n", r.impl, r.op, r.ns, r.mbps)
		}
		fmt.Printf("  go vs official: encode %.2fx, decode %.2fx\n\n",
			rows[1].ns/rows[0].ns, rows[3].ns/rows[2].ns)
	}
	if failures > 0 {
		fmt.Printf("FAIL: %d byte mismatches\n", failures)
		os.Exit(1)
	}
}

// verify encodes the same values through both implementations and checks the
// bytes and both decode paths. It returns the wire bytes.
func verify(repeats int) []byte {
	goMsg := fillPB(repeats)
	officialMsg := fillOfficial(repeats)
	want, err := proto.Marshal(officialMsg)
	if err != nil {
		fatalf("official encode: %v", err)
	}
	got, err := goMsg.Marshal()
	if err != nil {
		fatalf("go encode: %v", err)
	}
	if !bytes.Equal(got, want) {
		fatalf("go encode = %x, official = %x", got, want)
	}
	vt, err := officialMsg.MarshalVT()
	if err != nil {
		fatalf("vtproto encode: %v", err)
	}
	if !bytes.Equal(vt, want) {
		fatalf("vtproto encode = %x, official = %x", vt, want)
	}
	if size := goMsg.Size(); size != len(want) {
		fatalf("go Size() = %d, encoded %d bytes", size, len(want))
	}
	var goBack pbwire.Scalars3
	if err := goBack.Unmarshal(want); err != nil {
		fatalf("go decode: %v", err)
	}
	if re, err := goBack.Marshal(); err != nil || !bytes.Equal(re, want) {
		fatalf("go decode/re-encode mismatch: %v", err)
	}
	var officialBack official.Scalars3
	if err := proto.Unmarshal(want, &officialBack); err != nil {
		fatalf("official decode: %v", err)
	}
	if re, err := proto.Marshal(&officialBack); err != nil || !bytes.Equal(re, want) {
		fatalf("official decode/re-encode mismatch: %v", err)
	}
	return want
}

func mustMeasure(impl, op string, bytes int, fn func(*testing.B)) row {
	r, _ := measure(impl, op, bytes, fn)
	return r
}

func measure(impl, op string, bytes int, fn func(*testing.B)) (row, int) {
	res := testing.Benchmark(fn)
	return row{
		impl: impl,
		op:   op,
		ns:   float64(res.NsPerOp()),
		mbps: float64(bytes) / float64(res.NsPerOp()) * 1000,
	}, res.N
}

func fatalf(format string, args ...any) {
	fmt.Printf("FAIL "+format+"\n", args...)
	os.Exit(1)
}

// ---------------------------------------------------------------- fixtures

func fillPB(n int) *pbwire.Scalars3 {
	m := &pbwire.Scalars3{
		I32:    math.MinInt32,
		I64:    math.MaxInt64,
		U32:    math.MaxUint32,
		U64:    math.MaxUint64,
		S32:    math.MinInt32,
		S64:    math.MinInt64,
		Fx32:   0xdeadbeef,
		Fx64:   72623859790382856,
		Sfx32:  math.MinInt32,
		Sfx64:  math.MinInt64,
		F32:    float32(math.Inf(1)),
		F64:    -123456.789,
		Flag:   true,
		Str:    "hello",
		Color:  pbwire.Color3_GREEN,
		Inner:  &pbwire.Inner3{Name: "kid", Value: 42},
		OptI32: ptr(int32(-7)),
		OptStr: ptr("opt"),
	}
	m.Nums = make([]int32, n)
	m.Tags = make([]string, n)
	m.Inners = make([]pbwire.Inner3, n)
	m.Flags = make([]bool, n)
	for i := 0; i < n; i++ {
		m.Nums[i] = int32(i - 3)
		m.Tags[i] = "tag"
		m.Inners[i] = pbwire.Inner3{Name: "grandkid", Value: int32(i + 1)}
		m.Flags[i] = i%2 == 0
	}
	return m
}

func fillOfficial(n int) *official.Scalars3 {
	m := &official.Scalars3{
		I32:    math.MinInt32,
		I64:    math.MaxInt64,
		U32:    math.MaxUint32,
		U64:    math.MaxUint64,
		S32:    math.MinInt32,
		S64:    math.MinInt64,
		Fx32:   0xdeadbeef,
		Fx64:   72623859790382856,
		Sfx32:  math.MinInt32,
		Sfx64:  math.MinInt64,
		F32:    float32(math.Inf(1)),
		F64:    -123456.789,
		Flag:   true,
		Str:    "hello",
		Color:  official.Color3_COLOR3_GREEN,
		Inner:  &official.Inner3{Name: "kid", Value: 42},
		OptI32: ptr(int32(-7)),
		OptStr: ptr("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &official.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func ptr[T any](v T) *T { return &v }
