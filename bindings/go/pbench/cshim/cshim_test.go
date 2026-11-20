package cshim

import (
	"bytes"
	"fmt"
	"math"
	"runtime"
	"testing"
	"unsafe"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
	shimlayout "pbwire/pbench/cshim/layout"
	"pbwire/tests/gen/matrix3"
	"pbwire/tests/gen_oracle/matrix3pb"
)

type tier struct {
	label string
	n     int
	want  int
}

var tiers = []tier{
	{"small", 0, 136},
	{"medium", 1, 174},
	{"large", 100, 2469},
	{"xlarge", 3000, 74913},
}

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

func literalMessage(n int) *matrix3.Scalars3 {
	m := &matrix3.Scalars3{
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
		Color:  matrix3.Color3_COLOR3_GREEN,
		Inner:  &matrix3.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7),
		OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func checkStruct(label string, got *matrix3.Scalars3, want *matrix3pb.Scalars3) error {
	bad := func(what string) error {
		return fmt.Errorf("%s: decode lost %s", label, what)
	}
	for _, c := range []struct {
		name     string
		got, exp bool
	}{
		{"i32", got.I32 == want.I32, true},
		{"i64", got.I64 == want.I64, true},
		{"u32", got.U32 == want.U32, true},
		{"u64", got.U64 == want.U64, true},
		{"s32", got.S32 == want.S32, true},
		{"s64", got.S64 == want.S64, true},
		{"fx32", got.Fx32 == want.Fx32, true},
		{"fx64", got.Fx64 == want.Fx64, true},
		{"sfx32", got.Sfx32 == want.Sfx32, true},
		{"sfx64", got.Sfx64 == want.Sfx64, true},
		{"f32", got.F32 == want.F32, true},
		{"f64", got.F64 == want.F64, true},
		{"flag", got.Flag == want.Flag, true},
		{"str", got.Str == want.Str, true},
		{"color", int32(got.Color) == int32(want.Color), true},
		{"inner.name", got.Inner.GetName() == want.Inner.GetName(), true},
		{"inner.value", got.Inner.GetValue() == want.Inner.GetValue(), true},
		{"opt_i32", sameI32(got.OptI32, want.OptI32), true},
		{"opt_str", sameStr(got.OptStr, want.OptStr), true},
	} {
		if c.got != c.exp {
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

func TestGate(t *testing.T) {
	c, err := New()
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if c.Name() != "v2" {
		t.Fatalf("Name = %q", c.Name())
	}

	for _, tr := range tiers {
		t.Run(tr.label, func(t *testing.T) {
			want := fillOfficial(tr.n)
			ref, err := proto.Marshal(want)
			if err != nil {
				t.Fatal(err)
			}
			if len(ref) != tr.want {
				t.Fatalf("reference wrote %d bytes, want %d", len(ref), tr.want)
			}

			got, err := c.Unmarshal(ref)
			if err != nil {
				t.Fatalf("Unmarshal: %v", err)
			}
			var official matrix3pb.Scalars3
			if err := proto.Unmarshal(ref, &official); err != nil {
				t.Fatal(err)
			}
			if err := checkStruct(tr.label, got, &official); err != nil {
				t.Fatal(err)
			}

			out, err := c.Marshal(got)
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if !bytes.Equal(out, ref) {
				t.Fatalf("re-encode differs: %d vs %d bytes", len(out), len(ref))
			}

			lit := literalMessage(tr.n)
			out, err = c.Marshal(lit)
			if err != nil {
				t.Fatalf("Marshal(literals): %v", err)
			}
			if !bytes.Equal(out, ref) {
				t.Fatalf("literal encode differs: %d vs %d bytes",
					len(out), len(ref))
			}
		})
	}
}

// Decoded strings and slices must be Go-owned: a second decode into the same
// codec reuses its arena and accumulator buffers, so the first result must
// still hold its own values afterwards.
func TestDecodeOwnership(t *testing.T) {
	c, err := New()
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	firstRef, err := proto.Marshal(fillOfficial(3000))
	if err != nil {
		t.Fatal(err)
	}
	first, err := c.Unmarshal(firstRef)
	if err != nil {
		t.Fatal(err)
	}

	other := fillOfficial(5)
	other.Str = "different"
	other.OptStr = strp("different-opt")
	other.Tags[0] = "different-tag"
	other.Inners[0].Name = "different-kid"
	otherRef, err := proto.Marshal(other)
	if err != nil {
		t.Fatal(err)
	}
	// Force a collection before the second decode: the first result's string
	// headers point into its own Go arena and must keep that arena alive.
	runtime.GC()
	if _, err := c.Unmarshal(otherRef); err != nil {
		t.Fatal(err)
	}
	runtime.GC()

	if first.Str != "hello" {
		t.Fatalf("first Str changed to %q", first.Str)
	}
	if first.OptStr == nil || *first.OptStr != "opt" {
		t.Fatalf("first OptStr changed")
	}
	if len(first.Tags) != 3000 || first.Tags[0] != "tag" ||
		first.Tags[2999] != "tag" {
		t.Fatalf("first Tags changed")
	}
	if len(first.Inners) != 3000 || first.Inners[0].Name != "grandkid" ||
		first.Inners[2999].Value != 3000 {
		t.Fatalf("first Inners changed")
	}
	if len(first.Nums) != 3000 || first.Nums[0] != -3 ||
		first.Nums[2999] != 2996 {
		t.Fatalf("first Nums changed")
	}
	if len(first.Flags) != 3000 || first.Flags[0] != true ||
		first.Flags[1] != false {
		t.Fatalf("first Flags changed")
	}
	if first.Inner == nil || first.Inner.Name != "kid" || first.Inner.Value != 42 {
		t.Fatalf("first Inner changed")
	}
}

func TestAdversarial(t *testing.T) {
	c, err := New()
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	ref, err := proto.Marshal(fillOfficial(1))
	if err != nil {
		t.Fatal(err)
	}

	empty := []byte{}
	cases := []struct {
		name string
		data []byte
		err  bool
	}{
		{"empty", empty, false},
		{"field-zero", []byte{0x00}, true},
		{"wire-type-6", []byte{0x0e}, true},
		{"wire-type-7", []byte{0x2f}, true},
		{"truncated-at-end", ref[:len(ref)-1], true},
		{"truncated-length", []byte{0x72, 0x05}, true},
		{"huge-length", []byte{0x72, 0x80, 0x80, 0x80, 0x80, 0x20}, true},
		{"overlong-varint", []byte{0x08, 0x80, 0x80, 0x80, 0x80,
			0x80, 0x80, 0x80, 0x80, 0x80, 0x02}, true},
		{"end-group", []byte{0xa4, 0x06}, true},
	}
	for _, tc := range cases {
		m, err := c.Unmarshal(tc.data)
		if tc.err && err == nil {
			t.Fatalf("%s: expected error, decoded %+v", tc.name, m)
		}
		if !tc.err && err != nil {
			t.Fatalf("%s: unexpected error: %v", tc.name, err)
		}
	}

	// Every prefix must either decode or return an error, never panic.
	for i := 0; i <= len(ref); i++ {
		_, _ = c.Unmarshal(ref[:i])
	}

	// Depth bomb: 300 nested field-16 messages.
	bomb := []byte{}
	for i := 0; i < 300; i++ {
		tag := []byte{0x82, 0x01} // field 16, wire 2
		var nb []byte
		nb = append(tag, byte(len(bomb)))
		bomb = append(nb, bomb...)
	}
	if _, err := c.Unmarshal(bomb); err == nil {
		t.Fatal("depth bomb decoded without error")
	}

	// Unknown group is skipped, not rejected.
	withGroup := append([]byte{}, ref...)
	withGroup = append(withGroup, 0xa3, 0x06, 0x08, 0x05, 0xa4, 0x06)
	m, err := c.Unmarshal(withGroup)
	if err != nil {
		t.Fatalf("unknown group: %v", err)
	}
	if m.Str != "hello" || m.I32 != math.MinInt32 {
		t.Fatalf("unknown group decode lost fields")
	}
}

var (
	sinkBytes  []byte
	sinkStruct *matrix3.Scalars3
)

func BenchmarkMarshal(b *testing.B) {
	c, err := New()
	if err != nil {
		b.Fatal(err)
	}
	defer c.Close()
	for _, tr := range tiers {
		tr := tr
		b.Run(tr.label, func(b *testing.B) {
			m := literalMessage(tr.n)
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				out, err := c.Marshal(m)
				if err != nil {
					b.Fatal(err)
				}
				sinkBytes = out
			}
		})
	}
}

func BenchmarkUnmarshal(b *testing.B) {
	c, err := New()
	if err != nil {
		b.Fatal(err)
	}
	defer c.Close()
	for _, tr := range tiers {
		tr := tr
		b.Run(tr.label, func(b *testing.B) {
			ref, err := proto.Marshal(fillOfficial(tr.n))
			if err != nil {
				b.Fatal(err)
			}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				m, err := c.Unmarshal(ref)
				if err != nil {
					b.Fatal(err)
				}
				sinkStruct = m
			}
		})
	}
}

// ---------------------------------------------------------------------------
// C codec shapes beyond the matrix3 subset: bytes, generalized explicit
// presence, and required fields. A scratch struct plus hand-built layout
// rows drive the C codec directly (no matrix3 fixup involved); the oracle
// is dynamicpb over a proto2 descriptor, an independent implementation.
// ---------------------------------------------------------------------------

type scratchCshim struct {
	Raw  []byte
	Opt  *int64
	Flg  *bool
	Req  int32
	Zero *int32
}

func scratchDescriptor(t *testing.T) protoreflect.MessageDescriptor {
	t.Helper()
	str := func(s string) *string { return &s }
	i32 := func(n int32) *int32 { return &n }
	fd := &descriptorpb.FileDescriptorProto{
		Syntax:  str("proto2"),
		Name:    str("scratch.proto"),
		Package: str("scratch"),
		MessageType: []*descriptorpb.DescriptorProto{{
			Name: str("S"),
			Field: []*descriptorpb.FieldDescriptorProto{
				{Name: str("raw"), Number: i32(1), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_BYTES.Enum()},
				{Name: str("opt"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT64.Enum()},
				{Name: str("flg"), Number: i32(3), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_BOOL.Enum()},
				{Name: str("req"), Number: i32(4), Label: descriptorpb.FieldDescriptorProto_LABEL_REQUIRED.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
				{Name: str("zero"), Number: i32(5), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
			},
		}},
	}
	file, err := protodesc.NewFile(fd, nil)
	if err != nil {
		t.Fatal(err)
	}
	return file.Messages().ByName("S")
}

func scratchOracle(t *testing.T, md protoreflect.MessageDescriptor, raw []byte, opt *int64, flg *bool, req int32, zero *int32) []byte {
	t.Helper()
	m := dynamicpb.NewMessage(md)
	byNum := func(n protoreflect.FieldNumber) protoreflect.FieldDescriptor {
		return md.Fields().ByNumber(n)
	}
	if raw != nil {
		m.Set(byNum(1), protoreflect.ValueOfBytes(raw))
	}
	if opt != nil {
		m.Set(byNum(2), protoreflect.ValueOfInt64(*opt))
	}
	if flg != nil {
		m.Set(byNum(3), protoreflect.ValueOfBool(*flg))
	}
	m.Set(byNum(4), protoreflect.ValueOfInt32(req))
	if zero != nil {
		m.Set(byNum(5), protoreflect.ValueOfInt32(*zero))
	}
	// Deterministic: plain dynamicpb marshal walks an internal map, so
	// its field order is unstable run to run (see codec_test.go).
	wire, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	return wire
}

func scratchRows() (rows []shimlayout.Row, subrows []shimlayout.Row, baseSize, subSize uintptr) {
	var s scratchCshim
	row := func(fnum uint32, kind shimlayout.Kind, wire shimlayout.Wire, offset, has uint32, explicit, required bool) shimlayout.Row {
		return shimlayout.Row{
			Fnum: fnum, Kind: kind, Wire: wire,
			Offset: offset, Has: has,
			Explicit: explicit, Required: required,
			Sub: -1,
		}
	}
	offRaw := uint32(unsafe.Offsetof(s.Raw))
	offOpt := uint32(unsafe.Offsetof(s.Opt))
	offFlg := uint32(unsafe.Offsetof(s.Flg))
	offReq := uint32(unsafe.Offsetof(s.Req))
	offZero := uint32(unsafe.Offsetof(s.Zero))
	rows = []shimlayout.Row{
		// bytes: length-delimited like strings, distinct kind for fixup.
		{Fnum: 1, Kind: shimlayout.KindBytes, Wire: shimlayout.WireLen, Offset: offRaw, Sub: -1},
		// proto2 optional int64: explicit presence via pointer.
		row(2, shimlayout.KindInt64, shimlayout.WireVarint, offOpt, offOpt, true, false),
		// proto2 optional bool set to false: present, encodes zero.
		row(3, shimlayout.KindBool, shimlayout.WireVarint, offFlg, offFlg, true, false),
		// required int32: zero still emits.
		row(4, shimlayout.KindInt32, shimlayout.WireVarint, offReq, 0, false, true),
		// unset optional: omitted, the control.
		row(5, shimlayout.KindInt32, shimlayout.WireVarint, offZero, offZero, true, false),
	}
	// The constructor requires a non-empty sub-table; the scratch message
	// has no nesting, so a single dummy scalar row satisfies the contract.
	subrows = []shimlayout.Row{
		{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: 0, Sub: -1},
	}
	return rows, subrows, unsafe.Sizeof(s), 8
}

// testEncode drives the codec without the matrix3 fixup: the scratch
// struct needs none, re-encoding raw C state is byte-stable.
func testEncode(t *testing.T, c *Codec, m *scratchCshim) []byte {
	t.Helper()
	out, err := c.encodeRaw(unsafe.Pointer(m))
	runtime.KeepAlive(m)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func testDecode(t *testing.T, c *Codec, m *scratchCshim, wire []byte) {
	t.Helper()
	if _, err := c.decodeRaw(unsafe.Pointer(m), wire); err != nil {
		t.Fatal(err)
	}
	runtime.KeepAlive(m)
}

func TestCshimCodecBytesExplicit(t *testing.T) {
	md := scratchDescriptor(t)
	seven := int64(7)
	fals := false
	// raw set, opt set, flg present-but-false, req zero-but-required,
	// zero unset (omitted, the control).
	ref := scratchOracle(t, md, []byte("hi"), &seven, &fals, 0, nil)

	rows, subrows, baseSize, subSize := scratchRows()
	c, err := newCodecFromLayout(rows, subrows, baseSize, subSize)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	m := &scratchCshim{Raw: []byte("hi"), Opt: &seven, Flg: &fals, Req: 0}
	got := testEncode(t, c, m)
	if !bytes.Equal(got, ref) {
		t.Fatalf("encode differs: %x vs %x", got, ref)
	}

	// Reverse: our bytes parse as the same dynamic message.
	want := dynamicpb.NewMessage(md)
	byNum := func(n protoreflect.FieldNumber) protoreflect.FieldDescriptor {
		return md.Fields().ByNumber(n)
	}
	want.Set(byNum(1), protoreflect.ValueOfBytes([]byte("hi")))
	want.Set(byNum(2), protoreflect.ValueOfInt64(seven))
	want.Set(byNum(3), protoreflect.ValueOfBool(fals))
	want.Set(byNum(4), protoreflect.ValueOfInt32(0))
	rt := dynamicpb.NewMessage(md)
	if err := proto.Unmarshal(got, rt); err != nil {
		t.Fatalf("oracle reparse: %v", err)
	}
	if !proto.Equal(want, rt) {
		t.Fatal("oracle reparse mismatch")
	}

	// Decode writes raw C-backed state; re-encode must be byte-stable
	// without any Go fixup, and typed reads verify the values.
	back := &scratchCshim{}
	testDecode(t, c, back, ref)
	runtime.KeepAlive(ref)
	if back.Raw == nil || string(back.Raw) != "hi" {
		t.Fatalf("bytes field lost: %q", back.Raw)
	}
	if back.Opt == nil || *back.Opt != 7 {
		t.Fatalf("explicit int64 lost: %+v", back.Opt)
	}
	if back.Flg == nil || *back.Flg != false {
		t.Fatalf("explicit bool lost: %+v", back.Flg)
	}
	if back.Req != 0 {
		t.Fatalf("required zero changed: %d", back.Req)
	}
	if back.Zero != nil {
		t.Fatalf("unset optional materialized: %+v", back.Zero)
	}
	if again := testEncode(t, c, back); !bytes.Equal(again, ref) {
		t.Fatalf("re-encode differs: %x vs %x", again, ref)
	}
}
