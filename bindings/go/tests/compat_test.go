// Generated-compat check for the Go binding (TESTSPEC T1–T6, vectors.json as
// the single source of truth):
//
//	T1  generated Marshal == pinned protoc reference bytes
//	T2  Unmarshal of the reference == vectors, field by field
//	T3  zero value marshals empty; explicit optional zero round-trips
//	T4  differential vs the pinned protobuf-go oracle: same bytes, same
//	    fields, cross-decode agrees
//	T5  one generic workflow runs against both libraries' messages with
//	    identical observable results (import swap only)
//	T6  errors are loud, not crashes: bad wire, unmarshal into nil
//
// Run: go test ./... (from bindings/go; needs build/libpbwire.a, see
// the root Makefile go-test target).
package tests

import (
	"bytes"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"testing"

	"google.golang.org/protobuf/proto"

	gen1 "pbwire/tests/gen/matrix3"
	genone "pbwire/tests/gen/oneof1"
	pb1 "pbwire/tests/gen_oracle/matrix3pb"
	pbone "pbwire/tests/gen_oracle/oneof1pb"
)

const oraclePin = "v1.36.12"

var vectors struct {
	Message string `json:"message"`
	Values  map[string]any
	Extra   []struct {
		Message string            `json:"message"`
		Values  map[string]string `json:"values"`
	} `json:"extra_messages"`
}

var reference []byte

func testdata(t *testing.T, rel string) string {
	t.Helper()
	return filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "..", "codegen", "testdata", rel)
}

func loadVectors(t *testing.T) {
	t.Helper()
	td := filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "..", "codegen", "testdata")
	raw, err := os.ReadFile(filepath.Join(td, "vectors.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(raw, &vectors); err != nil {
		t.Fatal(err)
	}
	ref, err := os.ReadFile(filepath.Join(td, "matrix3.bin"))
	if err != nil {
		// vectors.json references ../../bindings/c/tests/data from the testdata dir
		ref, err = os.ReadFile(filepath.Join(td, "..", "..", "bindings", "c", "tests", "data", "matrix3.bin"))
		if err != nil {
			t.Fatal(err)
		}
	}
	reference = ref
}

func callerFile(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller")
	}
	return file
}

func buildScalars() *gen1.Scalars3 {
	oi, os := int32(-7), "opt"
	return &gen1.Scalars3{
		I32: -2147483648, I64: 9223372036854775807, U32: 4294967295, U64: 18446744073709551615,
		S32: -2147483648, S64: -9223372036854775808, Fx32: 3735928559, Fx64: 72623859790382856,
		Sfx32: -2147483648, Sfx64: -9223372036854775808, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: 2,
		Inner: &gen1.Inner3{Name: "kid", Value: 42},
		Nums:  []int32{1, -5, 300}, Tags: []string{"a", "bb"},
		Inners: []*gen1.Inner3{{Name: "x", Value: 1}, {Name: "y", Value: -2}},
		OptI32: &oi, OptStr: &os, Flags: []bool{true, false, true},
	}
}

func TestT1T2T3(t *testing.T) {
	loadVectors(t)
	wire, err := buildScalars().Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, reference) {
		t.Fatal("T1 generated Marshal != protoc reference bytes")
	}
	back := &gen1.Scalars3{}
	if err := back.Unmarshal(reference); err != nil {
		t.Fatal(err)
	}
	if back.GetI32() != math.MinInt32 || back.GetI64() != math.MaxInt64 ||
		back.GetU32() != 4294967295 || back.GetU64() != 18446744073709551615 ||
		back.GetS32() != math.MinInt32 || back.GetS64() != math.MinInt64 ||
		back.GetFx32() != 3735928559 || back.GetFx64() != 72623859790382856 ||
		back.GetSfx32() != math.MinInt32 || back.GetSfx64() != math.MinInt64 ||
		!math.IsInf(float64(back.GetF32()), 1) || back.GetF64() != -123456.789 ||
		!back.GetFlag() || back.GetStr() != "hello" || back.GetColor() != 2 ||
		back.GetInner().GetName() != "kid" || back.GetInner().GetValue() != 42 ||
		back.GetOptI32() != -7 || back.GetOptStr() != "opt" {
		t.Fatal("T2 scalar fields mismatch")
	}
	if back.OptI32 == nil || back.OptStr == nil {
		t.Fatal("T2 optional presence lost")
	}
	if got := back.GetNums(); len(got) != 3 || got[0] != 1 || got[1] != -5 || got[2] != 300 {
		t.Fatalf("T2 nums mismatch: %v", got)
	}
	if got := back.GetTags(); len(got) != 2 || got[0] != "a" || got[1] != "bb" {
		t.Fatalf("T2 tags mismatch: %v", got)
	}
	if inners := back.GetInners(); len(inners) != 2 || inners[0].GetName() != "x" || inners[1].GetValue() != -2 {
		t.Fatal("T2 inners mismatch")
	}
	if got := back.GetFlags(); len(got) != 3 || !got[0] || got[1] || !got[2] {
		t.Fatal("T2 flags mismatch")
	}
	empty, err := (&gen1.Scalars3{}).Marshal()
	if err != nil || len(empty) != 0 {
		t.Fatal("T3 empty message must encode empty")
	}
	z0 := int32(0)
	zb := &gen1.Scalars3{OptI32: &z0}
	zw, err := zb.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	zback := &gen1.Scalars3{}
	if err := zback.Unmarshal(zw); err != nil {
		t.Fatal(err)
	}
	if zback.OptI32 == nil || zback.GetOptI32() != 0 {
		t.Fatal("T3 explicit zero presence round-trips")
	}

	one := &genone.Oneof1{Name: "a", Kind: &genone.Oneof1_Text{Text: "hi"}}
	ow, err := one.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	ref1, err := os.ReadFile(testdata(t, "oneof1.bin"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(ow, ref1) {
		t.Fatal("T1 oneof Marshal != reference")
	}
	po := &pbone.Oneof1{Name: "a", Kind: &pbone.Oneof1_Text{Text: "hi"}}
	pow, err := proto.Marshal(po)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(pow, ref1) {
		t.Fatal("T4 oracle oneof != reference")
	}
	ob := &genone.Oneof1{}
	if err := ob.Unmarshal(ref1); err != nil {
		t.Fatal(err)
	}
	if ob.GetName() != "a" || ob.GetText() != "hi" {
		t.Fatal("T2 oneof decode mismatch")
	}
	nested := &genone.Oneof1{Name: "n", Kind: &genone.Oneof1_Inner_{Inner: &genone.Oneof1_Inner{V: 7}}}
	nw, err := nested.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	nb := &genone.Oneof1{}
	if err := nb.Unmarshal(nw); err != nil {
		t.Fatal(err)
	}
	if nb.GetInner().GetV() != 7 {
		t.Fatal("T2 nested oneof member mismatch")
	}
}

func buildOracle() *pb1.Scalars3 {
	oi, os := int32(-7), "opt"
	m := &pb1.Scalars3{
		I32: -2147483648, I64: 9223372036854775807, U32: 4294967295, U64: 18446744073709551615,
		S32: -2147483648, S64: -9223372036854775808, Fx32: 3735928559, Fx64: 72623859790382856,
		Sfx32: -2147483648, Sfx64: -9223372036854775808, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: 2,
		Inner: &pb1.Inner3{Name: "kid", Value: 42},
		Nums:  []int32{1, -5, 300}, Tags: []string{"a", "bb"},
		Inners: []*pb1.Inner3{{Name: "x", Value: 1}, {Name: "y", Value: -2}},
		OptI32: &oi, OptStr: &os, Flags: []bool{true, false, true},
	}
	return m
}

func protobufVersion() string {
	if bi, ok := debug.ReadBuildInfo(); ok {
		for _, d := range bi.Deps {
			if d.Path == "google.golang.org/protobuf" {
				return d.Version
			}
		}
	}
	return "unknown"
}

func TestT4Oracle(t *testing.T) {
	loadVectors(t)
	if v := protobufVersion(); v != oraclePin {
		t.Fatalf("oracle pin drifted (got %s); see testdata/ORACLES.md", v)
	}
	ow, err := proto.Marshal(buildOracle())
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(ow, reference) {
		t.Fatal("T4 oracle != reference")
	}
	fw, err := buildScalars().Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(fw, reference) {
		t.Fatal("T4 facade != reference")
	}
	// Cross-decode: oracle parses our bytes to the same fields.
	om := &pb1.Scalars3{}
	if err := proto.Unmarshal(fw, om); err != nil {
		t.Fatal(err)
	}
	fm := &gen1.Scalars3{}
	if err := fm.Unmarshal(ow); err != nil {
		t.Fatal(err)
	}
	check := func(name string, a, b any) {
		t.Helper()
		if a != b {
			t.Fatalf("T4 %s mismatch: %v != %v", name, a, b)
		}
	}
	check("i32", om.GetI32(), fm.GetI32())
	check("i64", om.GetI64(), fm.GetI64())
	check("u32", om.GetU32(), fm.GetU32())
	check("u64", om.GetU64(), fm.GetU64())
	check("flag", om.GetFlag(), fm.GetFlag())
	check("str", om.GetStr(), fm.GetStr())
	check("color", int32(om.GetColor()), int32(fm.GetColor()))
	check("inner", om.GetInner().GetName(), fm.GetInner().GetName())
	check("opt", om.GetOptI32(), fm.GetOptI32())
	if (om.OptI32 == nil) != (fm.OptI32 == nil) {
		t.Fatal("T4 optional presence agrees")
	}
	if len(om.GetNums()) != len(fm.GetNums()) || len(om.GetInners()) != len(fm.GetInners()) {
		t.Fatal("T4 repeated length agrees")
	}
}

// summary is the comparable business result for T5.
type summary struct {
	wire   string
	str    string
	i32    int32
	nums   []int32
	inner  string
	hasOpt bool
}

// workflow is written once against abstract operations; T instantiates it
// for each library, so both run literally the same steps.
func workflow[T any](new func() T, set func(T), marshal func(T) ([]byte, error),
	unmarshal func([]byte, T) error, get func(T) summary) summary {
	m := new()
	set(m)
	w, err := marshal(m)
	if err != nil {
		panic(err)
	}
	back := new()
	if err := unmarshal(w, back); err != nil {
		panic(err)
	}
	s := get(back)
	s.wire = string(w)
	return s
}

func TestT5Switch(t *testing.T) {
	loadVectors(t)
	a := workflow(
		func() *pb1.Scalars3 { return &pb1.Scalars3{} },
		func(m *pb1.Scalars3) {
			m.Str, m.I32, m.Nums = "kid", -5, []int32{1, 2}
			m.Inner = &pb1.Inner3{Name: "x", Value: 1}
		},
		func(m *pb1.Scalars3) ([]byte, error) { return proto.Marshal(m) },
		func(b []byte, m *pb1.Scalars3) error { return proto.Unmarshal(b, m) },
		func(m *pb1.Scalars3) summary {
			return summary{str: m.GetStr(), i32: m.GetI32(), nums: m.GetNums(),
				inner: m.GetInner().GetName(), hasOpt: m.OptI32 != nil}
		},
	)
	b := workflow(
		func() *gen1.Scalars3 { return &gen1.Scalars3{} },
		func(m *gen1.Scalars3) {
			m.Str, m.I32, m.Nums = "kid", -5, []int32{1, 2}
			m.Inner = &gen1.Inner3{Name: "x", Value: 1}
		},
		func(m *gen1.Scalars3) ([]byte, error) { return m.Marshal() },
		func(b []byte, m *gen1.Scalars3) error { return m.Unmarshal(b) },
		func(m *gen1.Scalars3) summary {
			return summary{str: m.GetStr(), i32: m.GetI32(), nums: m.GetNums(),
				inner: m.GetInner().GetName(), hasOpt: m.OptI32 != nil}
		},
	)
	if a.wire != b.wire || a.str != b.str || a.i32 != b.i32 || a.inner != b.inner || a.hasOpt != b.hasOpt {
		t.Fatalf("T5 mismatch:\n%+v\n%+v", a, b)
	}
	if len(a.nums) != len(b.nums) || a.nums[0] != b.nums[0] || a.nums[1] != b.nums[1] {
		t.Fatalf("T5 nums mismatch: %v vs %v", a.nums, b.nums)
	}
}

func TestT6Errors(t *testing.T) {
	loadVectors(t)
	if err := (&gen1.Scalars3{}).Unmarshal([]byte{0xff, 0xff, 0xff}); err == nil {
		t.Fatal("T6 bad wire must fail")
	}
	om := &pb1.Scalars3{}
	if err := proto.Unmarshal([]byte{0xff, 0xff, 0xff}, om); err == nil {
		t.Fatal("T6 oracle bad wire must fail")
	}
	if err := (func() error {
		var m *gen1.Scalars3
		return m.Unmarshal(reference)
	}()); err == nil {
		t.Fatal("T6 unmarshal into nil must fail")
	}
	if _, err := (*gen1.Scalars3)(nil).Marshal(); err != nil {
		t.Fatalf("T6 nil marshal must be empty, got %v", err)
	}
	one := &genone.Oneof1{Name: "n", Kind: &genone.Oneof1_Num{Num: 1}}
	w, err := one.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	ob := &genone.Oneof1{}
	if err := ob.Unmarshal(w); err != nil {
		t.Fatal(err)
	}
	if ob.GetNum() != 1 {
		t.Fatal("T6 oneof member round-trips")
	}
}
