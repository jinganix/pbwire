//go:build bench

// Go binding benchmark against the official protobuf-go generated code on
// the same matrix3.proto and the four standard tiers from BENCHMARKS.md.
//
// The same payload as every other binding: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes.
//
// One pbwire row: the thin generated model an application actually holds
// (Marshal()/Unmarshal() over the shared table engine, no cgo), struct in
// and struct out, against protobuf-go on the same task. The dynamic C core
// (prebuilt tree, unread handle) is not timed here: it answers a different
// question and lives in pbench/dync; see
// github.com/jinganix/pbwire/issues/41.
//
// Both sides produce identical bytes and the model decode is compared field by
// field against the oracle before anything is timed, so a speed number from a
// wrong codec is meaningless but never published.
//
// The descriptor set is built by bench.sh (protoc) and never committed. The
// two generated packages are the committed ones under tests/, so the
// baseline does not need protoc-gen-go.
//
//	./bench.sh          build, run
package benchmarks

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"runtime"
	"sync"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
	"pbwire/tests/gen/matrix3"
	"pbwire/tests/gen_oracle/matrix3pb"
)

type tier struct {
	label string
	n     int
	want  int
	floor int
}

var tiers = []tier{
	{"small", 0, 136, 50000},
	{"medium", 1, 174, 50000},
	{"large", 100, 2469, 5000},
	{"xlarge", 3000, 74913, 500},
}

const minPassSeconds = 0.25
const passes = 3

func i32p(v int32) *int32   { return &v }
func strp(v string) *string { return &v }

func sameI32(a, b *int32) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

func sameStr(a, b *string) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

func fillOfficial(n int) *matrix3pb.Scalars3 {
	m := &matrix3pb.Scalars3{
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
		Color:  matrix3pb.Color3_COLOR3_GREEN,
		Inner:  &matrix3pb.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7),
		OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3pb.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

// checkStruct compares every field of a pbwire decode against the protobuf-go
// decode of the same bytes: the Go core path is correct only if nothing was
// dropped on the way back into Go.
func checkStruct(label string, got *matrix3.Scalars3, want *matrix3pb.Scalars3) error {
	bad := func(what string) error {
		return fmt.Errorf("%s: struct decode lost %s", label, what)
	}
	for _, c := range []struct {
		name string
		ok   bool
	}{
		{"i32", got.I32 == want.I32},
		{"i64", got.I64 == want.I64},
		{"u32", got.U32 == want.U32},
		{"u64", got.U64 == want.U64},
		{"s32", got.S32 == want.S32},
		{"s64", got.S64 == want.S64},
		{"fx32", got.Fx32 == want.Fx32},
		{"fx64", got.Fx64 == want.Fx64},
		{"sfx32", got.Sfx32 == want.Sfx32},
		{"sfx64", got.Sfx64 == want.Sfx64},
		{"f32", got.F32 == want.F32},
		{"f64", got.F64 == want.F64},
		{"flag", got.Flag == want.Flag},
		{"str", got.Str == want.Str},
		{"color", int32(got.Color) == int32(want.Color)},
		{"inner.name", got.Inner.GetName() == want.Inner.GetName()},
		{"inner.value", got.Inner.GetValue() == want.Inner.GetValue()},
		{"opt_i32", sameI32(got.OptI32, want.OptI32)},
		{"opt_str", sameStr(got.OptStr, want.OptStr)},
	} {
		if !c.ok {
			return bad(c.name)
		}
	}
	if len(got.Nums) != len(want.Nums) || len(got.Tags) != len(want.Tags) ||
		len(got.Flags) != len(want.Flags) || len(got.Inners) != len(want.Inners) {
		return bad("repeated field lengths")
	}
	for i := range want.Nums {
		if got.Nums[i] != want.Nums[i] {
			return bad("nums[" + fmt.Sprint(i) + "]")
		}
		if got.Tags[i] != want.Tags[i] {
			return bad("tags[" + fmt.Sprint(i) + "]")
		}
		if got.Flags[i] != want.Flags[i] {
			return bad("flags[" + fmt.Sprint(i) + "]")
		}
		if got.Inners[i].Name != want.Inners[i].Name ||
			got.Inners[i].Value != want.Inners[i].Value {
			return bad("inners[" + fmt.Sprint(i) + "]")
		}
	}
	return nil
}

// The gate: every tier's official bytes land on the pinned size, the thin
// model encodes byte-identical bytes, and its decode keeps every field.
func gate(t tier) error {
	want := fillOfficial(t.n)
	reference, err := proto.Marshal(want)
	if err != nil {
		return err
	}
	if len(reference) != t.want {
		return fmt.Errorf("%s: official encoder wrote %d bytes, expected %d", t.label, len(reference), t.want)
	}

	structMsg := &matrix3.Scalars3{}
	if err := structMsg.Unmarshal(reference); err != nil {
		return fmt.Errorf("%s: model decode: %v", t.label, err)
	}
	if err := checkStruct(t.label, structMsg, want); err != nil {
		return err
	}
	structOut, err := structMsg.Marshal()
	if err != nil {
		return err
	}
	if !bytes.Equal(structOut, reference) {
		return fmt.Errorf("%s: pbwire-go and protobuf-go encode differently (%d vs %d bytes)",
			t.label, len(structOut), len(reference))
	}

	var official matrix3pb.Scalars3
	if err := proto.Unmarshal(reference, &official); err != nil {
		return err
	}
	if official.Str != "hello" || official.Inner.GetName() != "kid" ||
		official.Inner.GetValue() != 42 || len(official.Tags) != t.n ||
		len(official.Nums) != t.n || len(official.Inners) != t.n ||
		(t.n > 0 && official.Inners[t.n-1].GetValue() != int32(t.n)) {
		return fmt.Errorf("%s: official decoded values differ", t.label)
	}
	fmt.Printf("OK   %s parity: model decode field by field, byte identical, x%d (%d bytes)\n",
		t.label, t.n, len(reference))
	return nil
}

func TestMain(m *testing.M) {
	for _, t := range tiers {
		if err := gate(t); err != nil {
			fmt.Fprintln(os.Stderr, "FAIL:", err)
			os.Exit(1)
		}
	}
	os.Exit(m.Run())
}

func run(count int, op func()) float64 {
	start := time.Now()
	for i := 0; i < count; i++ {
		op()
	}
	return float64(time.Since(start).Nanoseconds()) / float64(count)
}

// measureParallel splits count ops across GOMAXPROCS goroutines and reports
// wall ns/op: what a server with shared-nothing messages sees. The closure
// must be goroutine-safe: decode builds a fresh message per op, encode only
// reads the prebuilt struct and discards the output (no shared sink).
func measureParallel(count int, op func()) float64 {
	n := runtime.GOMAXPROCS(0)
	if n < 1 {
		n = 1
	}
	per := count / n
	if per < 1 {
		per = 1
	}
	var wg sync.WaitGroup
	start := time.Now()
	for g := 0; g < n; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < per; i++ {
				op()
			}
		}()
	}
	wg.Wait()
	return float64(time.Since(start).Nanoseconds()) / float64(per*n)
}

// Every op calibrates independently from the floor: a slow op already exceeds
// the minimum at the floor and keeps it, a fast op doubles up.
func calibrate(op func(), floor int) int {
	count := floor
	if count < 1 {
		count = 1
	}
	for {
		start := time.Now()
		for i := 0; i < count; i++ {
			op()
		}
		if time.Since(start).Seconds() >= minPassSeconds || count >= 100000000 {
			return count
		}
		count *= 2
	}
}

func measure(floor int, op func()) float64 {
	runtime.GC() // a row inherits no heap growth from the row before it
	count := calibrate(op, floor)
	run(count, op) // warm-up
	best := float64(1e100)
	for p := 0; p < passes; p++ {
		if ns := run(count, op); ns < best {
			best = ns
		}
	}
	return best
}

func row(impl, op string, size int, ns float64, allocs float64, bop float64) {
	fmt.Printf("  %-17s %-7s %12.1f %10.1f %8.0f %10.0f\n", impl, op, ns, float64(size)/ns*1000.0, allocs, bop)
}

// allocStats measures allocations in passes separate from measure() so the
// measurement never perturbs the timed number: AllocsPerRun counts, and a
// MemStats.TotalAlloc delta over a calibrated count gives bytes/op.
func allocStats(op func()) (allocs float64, bop float64) {
	a := testing.AllocsPerRun(5, op)
	if a < 0 {
		a = 0
	}
	count := calibrate(op, 1000)
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	for i := 0; i < count; i++ {
		op()
	}
	runtime.ReadMemStats(&after)
	return float64(a), float64(after.TotalAlloc-before.TotalAlloc) / float64(count)
}

func TestMatrix3Bench(t *testing.T) {
	for _, tr := range tiers {
		official := fillOfficial(tr.n)
		reference, err := proto.Marshal(official)
		if err != nil {
			t.Fatal(err)
		}

		// The native object an application holds, built before the timer.
		structMsg := &matrix3.Scalars3{}
		if err := structMsg.Unmarshal(reference); err != nil {
			t.Fatal(err)
		}

		structEncode := measure(1, func() {
			sinkBytes, _ = structMsg.Marshal()
		})
		officialEncode := measure(tr.floor, func() {
			sinkBytes, _ = proto.Marshal(official)
		})
		structDecode := measure(1, func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		})
		officialDecode := measure(tr.floor, func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})
		structEncodeAllocs, structEncodeBop := allocStats(func() {
			sinkBytes, _ = structMsg.Marshal()
		})
		officialEncodeAllocs, officialEncodeBop := allocStats(func() {
			sinkBytes, _ = proto.Marshal(official)
		})
		structDecodeAllocs, structDecodeBop := allocStats(func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		})
		officialDecodeAllocs, officialDecodeBop := allocStats(func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})
		// Parallel rows: same ops, shared-nothing closures, wall time per op.
		structEncodePar := measureParallel(calibrate(func() {
			_, _ = structMsg.Marshal()
		}, 1), func() {
			_, _ = structMsg.Marshal()
		})
		officialEncodePar := measureParallel(calibrate(func() {
			_, _ = proto.Marshal(official)
		}, tr.floor), func() {
			_, _ = proto.Marshal(official)
		})
		structDecodePar := measureParallel(calibrate(func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		}, 1), func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		})
		officialDecodePar := measureParallel(calibrate(func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		}, tr.floor), func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})

		fmt.Printf("\n%s: %d bytes (repeat field x%d)\n", tr.label, len(reference), tr.n)
		fmt.Printf("  %-17s %-7s %12s %10s %8s %10s\n", "impl", "op", "ns/op", "MB/s", "allocs", "B/op")
		row("pbwire-go", "encode", len(reference), structEncode, structEncodeAllocs, structEncodeBop)
		row("protobuf-go", "encode", len(reference), officialEncode, officialEncodeAllocs, officialEncodeBop)
		row("pbwire-go", "decode", len(reference), structDecode, structDecodeAllocs, structDecodeBop)
		row("protobuf-go", "decode", len(reference), officialDecode, officialDecodeAllocs, officialDecodeBop)
		structEncodeParAllocs, structEncodeParBop := allocStats(func() {
			_, _ = structMsg.Marshal()
		})
		officialEncodeParAllocs, officialEncodeParBop := allocStats(func() {
			_, _ = proto.Marshal(official)
		})
		structDecodeParAllocs, structDecodeParBop := allocStats(func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		})
		officialDecodeParAllocs, officialDecodeParBop := allocStats(func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})
		row("pbwire-go", "encode-par", len(reference), structEncodePar, structEncodeParAllocs, structEncodeParBop)
		row("protobuf-go", "encode-par", len(reference), officialEncodePar, officialEncodeParAllocs, officialEncodeParBop)
		row("pbwire-go", "decode-par", len(reference), structDecodePar, structDecodeParAllocs, structDecodeParBop)
		row("protobuf-go", "decode-par", len(reference), officialDecodePar, officialDecodeParAllocs, officialDecodeParBop)
		fmt.Println()
	}
}

var sinkBytes []byte
