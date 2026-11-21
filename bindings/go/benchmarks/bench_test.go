//go:build bench

// One thin-model row plus one proxy row, against the official protobuf-go
// generated code on the same matrix3.proto and the four standard tiers
// from BENCHMARKS.md.
//
// The same payload as every other binding: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes.
//
// Two pbwire rows, named by the task they time:
//
//	pbwire-go-core   the thin generated model an application actually holds
//	                   (Marshal()/Unmarshal() over the shared table engine,
//	                   no cgo): struct in and struct out, the full task with
//	                   no prebuilt tree anywhere.
//	pbwire-go-proxy  the prebuilt task on the tree arm (held dynamic C
//	                   tree, re-encode and decode-and-free), published,
//	                   never the headline.
//
// The dynamic C core full task itself (struct -> C tree -> wire -> C tree ->
// struct through ImportModel/ExportModel per op) is not re-timed here: it
// already has that shape and is compared under pbench/dync; see
// bindings/go/pbench/README.md and
// github.com/jinganix/pbwire/issues/41.
//
// Both sides produce identical bytes and the model decode is compared field by
// field against the oracle before anything is timed, so a speed number from a
// wrong codec is meaningless but never published.
//
// The descriptor set is built by bench.sh (protoc) and never committed. The
// two generated packages are the committed ones under tests/, so the
// baseline does not need protoc-gen-go. The proxy row reaches the C core
// over cgo, so this harness needs a C toolchain and the repo static library
// (make go-bench builds it first); the thin model itself needs neither.
//
//	./bench.sh          build, run
package benchmarks

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
	"pbwire/pbench/dync/dynamic"
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

// proxyTyp is the matrix3 message type behind the proxy row, resolved once
// in TestMain from the pinned descriptor set. Each tier holds one tree of
// this type before its timer starts; every per-iteration op (re-encode the
// held tree, decode a fresh tree and free it) goes through shipped public
// API only (dynamic.Load, ImportModel, Encode, Decode, Close).
var proxyTyp *dynamic.MessageType

// loadProxyType opens the pinned descriptor set and resolves
// structtest.Scalars3, mirroring the tree arm's own loader (pbench/dync)
// without duplicating its conversion path: the bench holds trees, the arm
// owns the full task.
func loadProxyType() (*dynamic.Schema, *dynamic.MessageType, error) {
	_, thisFile, _, _ := runtime.Caller(0)
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(thisFile), "..", "pbench", "dyngo", "codec", "testdata", "matrix3.desc"))
	if err != nil {
		return nil, nil, fmt.Errorf("proxy: descriptor: %v", err)
	}
	schema, err := dynamic.Load(raw)
	if err != nil {
		return nil, nil, fmt.Errorf("proxy: load: %v", err)
	}
	typ, err := schema.Message("structtest.Scalars3")
	if err != nil {
		schema.Close()
		return nil, nil, fmt.Errorf("proxy: type: %v", err)
	}
	return schema, typ, nil
}

// The gate: every tier's official bytes land on the pinned size, the thin
// model encodes byte-identical bytes, and its decode keeps every field.
// The proxy arm gates beside it on the same bytes: a held tree re-encodes
// byte-identical bytes (twice, so hold-and-recode is stable), and a
// decode-and-free round trip with no materialization re-encodes
// byte-identical bytes too.
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

	// Proxy: hold one tree built from the same native struct the core row
	// encodes, then prove the two per-iteration ops reproduce the reference.
	held, err := proxyTyp.ImportModel(structMsg, matrix3.Scalars3Meta)
	if err != nil {
		return fmt.Errorf("%s: proxy import: %v", t.label, err)
	}
	defer held.Close()
	heldOut, err := held.Encode()
	if err != nil {
		return fmt.Errorf("%s: proxy encode: %v", t.label, err)
	}
	if !bytes.Equal(heldOut, reference) {
		return fmt.Errorf("%s: pbwire-go-proxy and protobuf-go encode differently (%d vs %d bytes)",
			t.label, len(heldOut), len(reference))
	}
	heldAgain, err := held.Encode()
	if err != nil || !bytes.Equal(heldAgain, reference) {
		return fmt.Errorf("%s: pbwire-go-proxy re-encode of the held tree differs", t.label)
	}
	fresh, err := proxyTyp.Decode(reference)
	if err != nil {
		return fmt.Errorf("%s: proxy decode: %v", t.label, err)
	}
	// No ExportModel: the proxy decode frees the tree with no
	// materialization, so the round trip back through Encode is the check.
	freshOut, err := fresh.Encode()
	fresh.Close()
	if err != nil {
		return fmt.Errorf("%s: proxy re-encode: %v", t.label, err)
	}
	if !bytes.Equal(freshOut, reference) {
		return fmt.Errorf("%s: pbwire-go-proxy decode round trip differs (%d vs %d bytes)",
			t.label, len(freshOut), len(reference))
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
	schema, typ, err := loadProxyType()
	if err != nil {
		fmt.Fprintln(os.Stderr, "FAIL:", err)
		os.Exit(1)
	}
	proxyTyp = typ
	for _, t := range tiers {
		if err := gate(t); err != nil {
			fmt.Fprintln(os.Stderr, "FAIL:", err)
			os.Exit(1)
		}
	}
	code := m.Run()
	typ.Close()
	schema.Close()
	os.Exit(code)
}

func run(count int, op func()) float64 {
	start := time.Now()
	for i := 0; i < count; i++ {
		op()
	}
	return float64(time.Since(start).Nanoseconds()) / float64(count)
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

func row(impl, op string, size int, ns float64) {
	fmt.Printf("  %-17s %-7s %12.1f %10.1f\n", impl, op, ns, float64(size)/ns*1000.0)
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
		// Proxy: one C tree held before the timer (the transcoder task).
		// The encode input is the same native struct the core row encodes;
		// the decode frees a fresh tree per iteration with no ExportModel.
		held, err := proxyTyp.ImportModel(structMsg, matrix3.Scalars3Meta)
		if err != nil {
			t.Fatal(err)
		}
		proxyEncode := measure(1, func() {
			sinkBytes, _ = held.Encode()
		})
		proxyDecode := measure(1, func() {
			proxyTree, _ := proxyTyp.Decode(reference)
			proxyTree.Close()
		})
		held.Close()
		structDecode := measure(1, func() {
			m := new(matrix3.Scalars3)
			_ = m.Unmarshal(reference)
		})
		officialDecode := measure(tr.floor, func() {
			var m matrix3pb.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})

		fmt.Printf("\n%s: %d bytes (repeat field x%d)\n", tr.label, len(reference), tr.n)
		fmt.Printf("  %-17s %-7s %12s %10s\n", "impl", "op", "ns/op", "MB/s")
		row("pbwire-go-core", "encode", len(reference), structEncode)
		row("pbwire-go-proxy", "encode", len(reference), proxyEncode)
		row("protobuf-go", "encode", len(reference), officialEncode)
		row("pbwire-go-core", "decode", len(reference), structDecode)
		row("pbwire-go-proxy", "decode", len(reference), proxyDecode)
		row("protobuf-go", "decode", len(reference), officialDecode)
		fmt.Println()
	}
}

var sinkBytes []byte
