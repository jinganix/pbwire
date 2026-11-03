// Wire-format self-check for generated code: every case is pinned protoc
// output (`protoc --encode` from the checked-in .txt vectors), so this file
// proves the generated codecs write the reference bytes, not just that they
// round-trip with themselves.
package tests

import (
	"bytes"
	"errors"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	pbwire "pbwire/runtime"
	edgetest "pbwire/tests/gen/edgetest"
	structtest "pbwire/tests/gen/structtest"
)

func read(t *testing.T, name string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(testDir(), name))
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func testDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(file)
}

func ptr[T any](v T) *T { return &v }

// ---------------------------------------------------------------- matrix3

func scalars3() *structtest.Scalars3 {
	return &structtest.Scalars3{
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
		Color:  structtest.Color3_GREEN,
		Inner:  &structtest.Inner3{Name: "kid", Value: 42},
		Nums:   []int32{1, -5, 300},
		Tags:   []string{"a", "bb"},
		Inners: []structtest.Inner3{{Name: "x", Value: 1}, {Name: "y", Value: -2}},
		OptI32: ptr(int32(-7)),
		OptStr: ptr("opt"),
		Flags:  []bool{true, false, true},
	}
}

func TestMatrix3Reference(t *testing.T) {
	want := read(t, "../../c/tests/data/matrix3.bin")
	m := scalars3()
	if got := m.Size(); got != len(want) {
		t.Fatalf("Size = %d, reference is %d bytes", got, len(want))
	}
	got, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("Marshal = %x, want %x", got, want)
	}

	var back structtest.Scalars3
	if err := back.Unmarshal(want); err != nil {
		t.Fatal(err)
	}
	if !equalScalars3(&back, m) {
		t.Fatalf("decoded message differs:\n got %+v\nwant %+v", back, *m)
	}
	again, err := back.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, want) {
		t.Fatalf("re-Marshal = %x, want %x", again, want)
	}
}

func equalScalars3(a, b *structtest.Scalars3) bool {
	return a.I32 == b.I32 && a.I64 == b.I64 && a.U32 == b.U32 && a.U64 == b.U64 &&
		a.S32 == b.S32 && a.S64 == b.S64 && a.Fx32 == b.Fx32 && a.Fx64 == b.Fx64 &&
		a.Sfx32 == b.Sfx32 && a.Sfx64 == b.Sfx64 && a.F32 == b.F32 && a.F64 == b.F64 &&
		a.Flag == b.Flag && a.Str == b.Str && a.Color == b.Color &&
		a.Inner != nil && b.Inner != nil && *a.Inner == *b.Inner &&
		equalInt32s(a.Nums, b.Nums) && equalStrings(a.Tags, b.Tags) &&
		equalInners(a.Inners, b.Inners) &&
		*a.OptI32 == *b.OptI32 && *a.OptStr == *b.OptStr &&
		equalBools(a.Flags, b.Flags)
}

func equalInt32s(a, b []int32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func equalBools(a, b []bool) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func equalInners(a, b []structtest.Inner3) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------- matrix2

func scalars2() *structtest.Scalars2 {
	return &structtest.Scalars2{
		Id:         ptr(int32(99)),
		I32:        ptr(int32(-5)),
		S64:        ptr(int64(-1)),
		Fx32:       ptr(uint32(305419896)),
		Dbl:        ptr(2.5),
		Flag:       ptr(true),
		Str:        ptr("hi"),
		Shade:      ptr(structtest.Shade_NEG),
		Nums:       []int32{4, 5},
		PackedNums: []int32{6, 7, 8},
		Inner:      &structtest.Inner2{Name: ptr("req"), Value: ptr(int32(9))},
		Inners: []structtest.Inner2{
			{Name: ptr("a")},
			{Name: ptr("b"), Value: ptr(int32(-3))},
		},
	}
}

func TestMatrix2Reference(t *testing.T) {
	want := read(t, "../../c/tests/data/matrix2.bin")
	m := scalars2()
	if got := m.Size(); got != len(want) {
		t.Fatalf("Size = %d, reference is %d bytes", got, len(want))
	}
	got, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("Marshal = %x, want %x", got, want)
	}
	var back structtest.Scalars2
	if err := back.Unmarshal(want); err != nil {
		t.Fatal(err)
	}
	again, err := back.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, want) {
		t.Fatalf("re-Marshal = %x, want %x", again, want)
	}
	if back.I32 == nil || *back.I32 != -5 {
		t.Fatal("absent optional fields must stay nil: proto2 defaults are not materialized")
	}
}

// ---------------------------------------------------------------- edges

func edges() *edgetest.Edges {
	return &edgetest.Edges{
		Data:     []byte("abc"),
		OptData:  []byte{},
		Chunks:   [][]byte{[]byte("x"), {}},
		OptNum:   ptr(int32(-9)),
		Packed:   []int32{1, -1, 300},
		Unpacked: []int32{2, -2, 300},
		F32:      math.Float32frombits(0x80000000), // -0.0, which proto3 must emit
		F64:      -123456.789,
		U64:      math.MaxUint64,
		Str:      "edge",
		Flags:    []bool{true, false, true},
		Floats:   []float32{0, math.Float32frombits(0x80000000), 1.5},
		Doubles:  []float64{0, math.Copysign(0, -1), -2.5},
	}
}

func TestEdgesReference(t *testing.T) {
	want := read(t, "data/edges.bin")
	m := edges()
	if got := m.Size(); got != len(want) {
		t.Fatalf("Size = %d, reference is %d bytes", got, len(want))
	}
	got, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("Marshal = %x, want %x", got, want)
	}
	var back edgetest.Edges
	if err := back.Unmarshal(want); err != nil {
		t.Fatal(err)
	}
	if back.OptData == nil {
		t.Fatal("present empty optional bytes decoded as absent")
	}
	if back.Chunks[1] == nil {
		t.Fatal("present empty repeated bytes decoded as nil")
	}
	again, err := back.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, want) {
		t.Fatalf("re-Marshal = %x, want %x", again, want)
	}
}

func TestPresence(t *testing.T) {
	if got, err := (&edgetest.Edges{}).Marshal(); err != nil || len(got) != 0 {
		t.Fatalf("zero message = %x, %v; want no bytes", got, err)
	}
	cases := []struct {
		name string
		m    *edgetest.Edges
		want []byte
	}{
		{"implicit empty bytes stays absent", &edgetest.Edges{Data: []byte{}}, nil},
		{"explicit empty bytes is emitted", &edgetest.Edges{OptData: []byte{}}, []byte{0x12, 0x00}},
		{"repeated empty bytes is emitted", &edgetest.Edges{Chunks: [][]byte{{}}}, []byte{0x1a, 0x00}},
		{"negative zero float is emitted", &edgetest.Edges{F32: math.Float32frombits(0x80000000)}, []byte{0x3d, 0x00, 0x00, 0x00, 0x80}},
		{"positive zero float is not emitted", &edgetest.Edges{F32: 0}, nil},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got, err := tc.m.Marshal()
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, tc.want) {
				t.Fatalf("Marshal = %x, want %x", got, tc.want)
			}
			var back edgetest.Edges
			if err := back.Unmarshal(got); err != nil {
				t.Fatal(err)
			}
			again, err := back.Marshal()
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(again, got) {
				t.Fatalf("round trip = %x, want %x", again, got)
			}
		})
	}
}

// ---------------------------------------------------------------- hostile input

func TestUnknownAndMistypedFieldsAreSkipped(t *testing.T) {
	want := read(t, "../../c/tests/data/matrix3.bin")

	// An unknown varint field and field 1's number carrying the wrong wire type
	// must not disturb the known fields, and must not survive a re-encode: the
	// generated struct has no storage for them.
	hostile := append([]byte{}, want...)
	hostile = pbwire.AppendVarint(hostile, 100<<3|0)
	hostile = pbwire.AppendVarint(hostile, 7)
	hostile = append(hostile, 0x0a, 0x01, 'z') // field 1, wire type 2 (i32 is varint)

	var m structtest.Scalars3
	if err := m.Unmarshal(hostile); err != nil {
		t.Fatal(err)
	}
	if m.I32 != math.MinInt32 {
		t.Fatalf("i32 = %d after unknown+wrong-wire fields", m.I32)
	}
	got, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("re-Marshal = %x, want %x", got, want)
	}
}

func TestTruncatedInputNeverPanics(t *testing.T) {
	for _, name := range []string{"../../c/tests/data/matrix3.bin", "../../c/tests/data/matrix2.bin", "data/edges.bin"} {
		full := read(t, name)
		for i := 0; i < len(full); i++ {
			var m structtest.Scalars3
			_ = m.Unmarshal(full[:i])
			var m2 structtest.Scalars2
			_ = m2.Unmarshal(full[:i])
			var e edgetest.Edges
			_ = e.Unmarshal(full[:i])
		}
	}
}

func TestPackedAndUnpackedBothAccepted(t *testing.T) {
	var want []byte
	want = append(want, 0x08, 0x63) // id = 99 (required)
	// field 9 (nums, [packed=false]): packed run on the wire
	want = append(want, 0x4a, 0x02, 0x04, 0x05)
	// field 10 (packed_nums, packed default): unpacked elements on the wire
	want = append(want, 0x50, 0x06, 0x50, 0x07, 0x50, 0x08)

	var m structtest.Scalars2
	if err := m.Unmarshal(want); err != nil {
		t.Fatal(err)
	}
	if !equalInt32s(m.Nums, []int32{4, 5}) || !equalInt32s(m.PackedNums, []int32{6, 7, 8}) {
		t.Fatalf("nums = %v, packed_nums = %v", m.Nums, m.PackedNums)
	}
}

func TestRequiredFields(t *testing.T) {
	if _, err := (&structtest.Scalars2{}).Marshal(); !errors.Is(err, pbwire.ErrRequired) {
		t.Fatalf("encode without id = %v, want ErrRequired", err)
	}
	if _, err := (&structtest.Inner2{}).Marshal(); !errors.Is(err, pbwire.ErrRequired) {
		t.Fatalf("encode without name = %v, want ErrRequired", err)
	}
	var m structtest.Scalars2
	if err := m.Unmarshal(nil); !errors.Is(err, pbwire.ErrRequired) {
		t.Fatalf("decode without id = %v, want ErrRequired", err)
	}
	// id present, optional inner present but missing its required name.
	wire := []byte{0x08, 0x63, 0x5a, 0x02, 0x10, 0x09}
	if err := m.Unmarshal(wire); !errors.Is(err, pbwire.ErrRequired) {
		t.Fatalf("decode with nameless inner = %v, want ErrRequired", err)
	}
	// The same bytes without the inner message are fine.
	if err := m.Unmarshal([]byte{0x08, 0x63}); err != nil {
		t.Fatalf("decode with id only = %v", err)
	}
}

func TestDuplicateSingularMessageMerges(t *testing.T) {
	wire := read(t, "../../c/tests/data/matrix2.bin")
	// Append a second inner {value: 7}; the first one carries name "req", so
	// the decoded message must hold both fields.
	wire = append(wire, 0x5a, 0x02, 0x10, 0x07)
	var m structtest.Scalars2
	if err := m.Unmarshal(wire); err != nil {
		t.Fatal(err)
	}
	if m.Inner == nil || m.Inner.Name == nil || *m.Inner.Name != "req" ||
		m.Inner.Value == nil || *m.Inner.Value != 7 {
		t.Fatalf("inner = %+v, want merged {name req, value 7}", m.Inner)
	}
}

func TestNestingLimit(t *testing.T) {
	// Encode: a chain deeper than MaxDepth must fail, not exhaust the stack.
	root := &edgetest.Node{Value: 0}
	cur := root
	for i := 1; i < pbwire.MaxDepth+10; i++ {
		cur.Next = &edgetest.Node{Value: int32(i)}
		cur = cur.Next
	}
	if _, err := root.Marshal(); !errors.Is(err, pbwire.ErrDepth) {
		t.Fatalf("deep chain = %v, want ErrDepth", err)
	}
	root.Size() // must not recurse past the cap

	// Encode: a cycle is caught by the same cap.
	cycle := &edgetest.Node{Value: 1}
	cycle.Next = cycle
	if _, err := cycle.Marshal(); !errors.Is(err, pbwire.ErrDepth) {
		t.Fatalf("cycle = %v, want ErrDepth", err)
	}

	// Decode: nested messages past the cap must be rejected, below it accepted.
	nest := func(levels int) []byte {
		var b []byte
		for i := 0; i < levels; i++ {
			next := pbwire.AppendVarint(nil, 2<<3|2)
			next = pbwire.AppendVarint(next, uint64(len(b)))
			b = append(next, b...)
		}
		return b
	}
	var node edgetest.Node
	if err := node.Unmarshal(nest(pbwire.MaxDepth + 1)); !errors.Is(err, pbwire.ErrDepth) {
		t.Fatalf("deep decode = %v, want ErrDepth", err)
	}
	if err := node.Unmarshal(nest(100)); err != nil {
		t.Fatalf("100-level decode = %v", err)
	}
}

func TestMarshalAppendKeepsPrefix(t *testing.T) {
	m := scalars3()
	want, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	prefix := []byte("keep-me")
	got, err := m.MarshalAppend(append([]byte{}, prefix...))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(got, prefix) || !bytes.Equal(got[len(prefix):], want) {
		t.Fatalf("MarshalAppend = %x", got)
	}
}
