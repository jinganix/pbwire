package engine

import (
	"bytes"
	"fmt"
	"reflect"
	"runtime"
	"sync"
	"testing"
	"unsafe"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
	shimlayout "pbwire/pbench/cshim/layout"
)

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
func testEncode(t *testing.T, c *Codec, m any) []byte {
	t.Helper()
	out, err := c.EncodeRaw(unsafe.Pointer(reflect.ValueOf(m).Pointer()))
	runtime.KeepAlive(m)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func testDecode(t *testing.T, c *Codec, m any, wire []byte) {
	t.Helper()
	if _, err := c.DecodeRaw(unsafe.Pointer(reflect.ValueOf(m).Pointer()), wire); err != nil {
		t.Fatal(err)
	}
	runtime.KeepAlive(m)
	runtime.KeepAlive(wire)
}

func TestCshimCodecBytesExplicit(t *testing.T) {
	md := scratchDescriptor(t)
	seven := int64(7)
	fals := false
	// raw set, opt set, flg present-but-false, req zero-but-required,
	// zero unset (omitted, the control).
	ref := scratchOracle(t, md, []byte("hi"), &seven, &fals, 0, nil)

	rows, subrows, baseSize, subSize := scratchRows()
	reg := []shimlayout.Msg{
		{Full: "scratch.S", Size: baseSize, Rows: rows},
		{Full: "scratch.Sub", Size: subSize, Rows: subrows},
	}
	c, err := NewCodecFromLayout(reg, "scratch.S")
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

// ---------------------------------------------------------------------------
// Groups and multi-level nesting. Root S holds a singular group, a repeated
// group and a two-level nested message; the oracle is dynamicpb over a
// proto2 descriptor. A mismatched end tag and a truncated group must both
// fail instead of decoding silently.
// ---------------------------------------------------------------------------

type scratchLeaf struct{ W int32 }

type scratchSub struct {
	Leaf *scratchLeaf
	N    int32
}

type scratchGrp struct {
	V int32
	S string
}

type scratchRep struct{ V int32 }

type scratchSubRep struct {
	N    int32
	Tags []string
}

type scratchG struct {
	Grp *scratchGrp
	Rep []*scratchRep
	Sub *scratchSub
}

func scratchGroupDescriptor(t *testing.T) protoreflect.MessageDescriptor {
	t.Helper()
	str := func(s string) *string { return &s }
	i32 := func(n int32) *int32 { return &n }
	f := func(name string, num int32, label descriptorpb.FieldDescriptorProto_Label, typ descriptorpb.FieldDescriptorProto_Type, typeName string) *descriptorpb.FieldDescriptorProto {
		fd := &descriptorpb.FieldDescriptorProto{
			Name: str(name), Number: i32(num), Label: label.Enum(), Type: typ.Enum(),
		}
		if typeName != "" {
			fd.TypeName = str(typeName)
		}
		return fd
	}
	opt := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	req := descriptorpb.FieldDescriptorProto_LABEL_REQUIRED
	_ = req
	rep := descriptorpb.FieldDescriptorProto_LABEL_REPEATED
	fd := &descriptorpb.FileDescriptorProto{
		Syntax:  str("proto2"),
		Name:    str("scratch3.proto"),
		Package: str("scratch3"),
		MessageType: []*descriptorpb.DescriptorProto{
			{Name: str("Leaf"), Field: []*descriptorpb.FieldDescriptorProto{
				f("w", 1, opt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			}},
			{Name: str("Sub"), Field: []*descriptorpb.FieldDescriptorProto{
				f("leaf", 1, opt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".scratch3.Leaf"),
				f("n", 2, opt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			}},
			{Name: str("S"), Field: []*descriptorpb.FieldDescriptorProto{
				f("grp", 1, opt, descriptorpb.FieldDescriptorProto_TYPE_GROUP, ".scratch3.S.Grp"),
				f("rep", 2, rep, descriptorpb.FieldDescriptorProto_TYPE_GROUP, ".scratch3.S.Rep"),
				f("sub", 3, opt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".scratch3.Sub"),
			},
				NestedType: []*descriptorpb.DescriptorProto{
					{Name: str("Grp"), Field: []*descriptorpb.FieldDescriptorProto{
						f("v", 1, opt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
						f("s", 2, opt, descriptorpb.FieldDescriptorProto_TYPE_STRING, ""),
					}},
					{Name: str("Rep"), Field: []*descriptorpb.FieldDescriptorProto{
						f("v", 1, opt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
					}},
				}},
		},
	}
	file, err := protodesc.NewFile(fd, nil)
	if err != nil {
		t.Fatal(err)
	}
	return file.Messages().ByName("S")
}

func scratchGroupRows() []shimlayout.Msg {
	return []shimlayout.Msg{
		{Full: "scratch3.S", Size: unsafe.Sizeof(scratchG{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindMessage, Wire: shimlayout.WireStartGroup, Group: true, Offset: uint32(unsafe.Offsetof(scratchG{}.Grp)), Sub: 1},
			{Fnum: 2, Kind: shimlayout.KindMessage, Wire: shimlayout.WireStartGroup, Group: true, Repeated: true, Offset: uint32(unsafe.Offsetof(scratchG{}.Rep)), Sub: 2},
			{Fnum: 3, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchG{}.Sub)), Sub: 3},
		}},
		{Full: "scratch3.Grp", Size: unsafe.Sizeof(scratchGrp{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchGrp{}.V)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchGrp{}.S)), Sub: -1},
		}},
		{Full: "scratch3.Rep", Size: unsafe.Sizeof(scratchRep{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchRep{}.V)), Sub: -1},
		}},
		{Full: "scratch3.Sub", Size: unsafe.Sizeof(scratchSub{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchSub{}.Leaf)), Sub: 4},
			{Fnum: 2, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchSub{}.N)), Sub: -1},
		}},
		{Full: "scratch3.Leaf", Size: unsafe.Sizeof(scratchLeaf{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchLeaf{}.W)), Sub: -1},
		}},
	}
}

func TestCshimCodecGroups(t *testing.T) {
	md := scratchGroupDescriptor(t)
	byNum := func(n protoreflect.FieldNumber) protoreflect.FieldDescriptor {
		return md.Fields().ByNumber(n)
	}
	build := func() *dynamicpb.Message {
		m := dynamicpb.NewMessage(md)
		leaf := dynamicpb.NewMessage(byNum(3).Message().Fields().ByNumber(1).Message())
		leaf.Set(leaf.Descriptor().Fields().ByNumber(1), protoreflect.ValueOfInt32(9))
		sub := dynamicpb.NewMessage(byNum(3).Message())
		sub.Set(sub.Descriptor().Fields().ByNumber(1), protoreflect.ValueOfMessage(leaf))
		sub.Set(sub.Descriptor().Fields().ByNumber(2), protoreflect.ValueOfInt32(5))
		grp := dynamicpb.NewMessage(byNum(1).Message())
		grp.Set(grp.Descriptor().Fields().ByNumber(1), protoreflect.ValueOfInt32(7))
		grp.Set(grp.Descriptor().Fields().ByNumber(2), protoreflect.ValueOfString("g"))
		m.Set(byNum(1), protoreflect.ValueOfMessage(grp))
		rep1 := dynamicpb.NewMessage(byNum(2).Message())
		rep1.Set(rep1.Descriptor().Fields().ByNumber(1), protoreflect.ValueOfInt32(1))
		rep2 := dynamicpb.NewMessage(byNum(2).Message())
		rep2.Set(rep2.Descriptor().Fields().ByNumber(1), protoreflect.ValueOfInt32(2))
		ml := m.Mutable(byNum(2)).List()
		ml.Append(protoreflect.ValueOfMessage(rep1))
		ml.Append(protoreflect.ValueOfMessage(rep2))
		m.Set(byNum(3), protoreflect.ValueOfMessage(sub))
		return m
	}
	want := build()
	ref, err := proto.MarshalOptions{Deterministic: true}.Marshal(want)
	if err != nil {
		t.Fatal(err)
	}

	c, err := NewCodecFromLayout(scratchGroupRows(), "scratch3.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	m := &scratchG{
		Grp: &scratchGrp{V: 7, S: "g"},
		Rep: []*scratchRep{{V: 1}, {V: 2}},
		Sub: &scratchSub{Leaf: &scratchLeaf{W: 9}, N: 5},
	}
	if got := testEncode(t, c, m); !bytes.Equal(got, ref) {
		t.Fatalf("encode differs: %x vs %x", got, ref)
	}

	// Reverse: our bytes parse as the same dynamic message.
	rt := dynamicpb.NewMessage(md)
	if err := proto.Unmarshal(ref, rt); err != nil {
		t.Fatalf("oracle reparse: %v", err)
	}
	if !proto.Equal(want, rt) {
		t.Fatal("oracle reparse mismatch")
	}

	// Decode writes raw C-backed state; typed reads verify it and
	// re-encoding must be byte-stable without any Go fixup.
	back := &scratchG{}
	testDecode(t, c, back, ref)
	runtime.KeepAlive(ref)
	if back.Grp == nil || back.Grp.V != 7 || back.Grp.S != "g" {
		t.Fatalf("group lost: %+v", back.Grp)
	}
	if len(back.Rep) != 2 || back.Rep[0].V != 1 || back.Rep[1].V != 2 {
		t.Fatalf("repeated group lost: %+v", back.Rep)
	}
	if back.Sub == nil || back.Sub.Leaf == nil || back.Sub.Leaf.W != 9 || back.Sub.N != 5 {
		t.Fatalf("nested message lost: %+v", back.Sub)
	}
	if again := testEncode(t, c, back); !bytes.Equal(again, ref) {
		t.Fatalf("re-encode differs: %x vs %x", again, ref)
	}

	// A mismatched end tag must fail, not skip.
	bad := append([]byte{}, ref...)
	if len(bad) < 7 || bad[6] != 0x0c {
		t.Fatalf("unexpected oracle prefix: %x", ref)
	}
	bad[6] = 0x14 // first group closed by field 2's end tag
	badBase := unsafe.Pointer(reflect.ValueOf(&scratchG{}).Pointer())
	if _, err := c.DecodeRaw(badBase, bad); err == nil {
		t.Fatal("mismatched end tag decoded without error")
	}
	runtime.KeepAlive(bad)
	// A truncated group must fail as well.
	if _, err := c.DecodeRaw(badBase, ref[:len(ref)-1]); err == nil {
		t.Fatal("truncated input decoded without error")
	}
}

// ---------------------------------------------------------------------------
// Oneof members via the itab table: the C codec matches the interface word
// with no Go work per call, including zero-valued members which still
// encode. Decode stages (member, payload) for the fixup; last occurrence
// wins. Message-member merge across split occurrences is a documented
// follow-up (single occurrences round-trip exactly).
// ---------------------------------------------------------------------------

type scratchOneofN struct{ N int32 }

type scratchOneofT struct{ T string }

type scratchOneofF struct{ F float32 }

type scratchOneofD struct{ D float64 }

type scratchOneofInner struct {
	V int32
	W int32
}

type scratchOneofW struct{ In *scratchOneofInner }

type scratchOneof struct {
	Name string
	Kind any
}

func scratchOneofDescriptor(t *testing.T) protoreflect.MessageDescriptor {
	t.Helper()
	str := func(s string) *string { return &s }
	i32 := func(n int32) *int32 { return &n }
	f := func(name string, num int32, typ descriptorpb.FieldDescriptorProto_Type, typeName string, oneof string) *descriptorpb.FieldDescriptorProto {
		fd := &descriptorpb.FieldDescriptorProto{
			Name: str(name), Number: i32(num), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: typ.Enum(),
		}
		if typeName != "" {
			fd.TypeName = str(typeName)
		}
		if oneof != "" {
			fd.OneofIndex = i32(0)
		}
		_ = oneof
		return fd
	}
	_ = f
	fd := &descriptorpb.FileDescriptorProto{
		Syntax:  str("proto3"),
		Name:    str("scratch4.proto"),
		Package: str("scratch4"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: str("Inner"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: str("v"), Number: i32(1), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
					{Name: str("w"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
				},
			},
			{
				Name: str("S"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: str("name"), Number: i32(1), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum()},
					{Name: str("num"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum(), OneofIndex: i32(0)},
					{Name: str("text"), Number: i32(3), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), OneofIndex: i32(0)},
					{Name: str("inner"), Number: i32(4), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: str(".scratch4.Inner"), OneofIndex: i32(0)},
					{Name: str("f"), Number: i32(5), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_FLOAT.Enum(), OneofIndex: i32(0)},
					{Name: str("d"), Number: i32(6), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_DOUBLE.Enum(), OneofIndex: i32(0)},
				},
				OneofDecl: []*descriptorpb.OneofDescriptorProto{{Name: str("kind")}},
			},
		},
	}
	file, err := protodesc.NewFile(fd, nil)
	if err != nil {
		t.Fatal(err)
	}
	return file.Messages().ByName("S")
}

func scratchOneofRows() []shimlayout.Msg {
	return []shimlayout.Msg{
		{Full: "scratch4.S", Size: unsafe.Sizeof(scratchOneof{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Name)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Kind)), Itab: shimlayout.ItabOf((*scratchOneofN)(nil)), Ogrp: 0, Sub: -1},
			{Fnum: 3, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Kind)), Itab: shimlayout.ItabOf((*scratchOneofT)(nil)), Ogrp: 0, Sub: -1},
			{Fnum: 4, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Kind)), Itab: shimlayout.ItabOf((*scratchOneofW)(nil)), Ogrp: 0, Sub: 1},
			{Fnum: 5, Kind: shimlayout.KindFloat, Wire: shimlayout.WireFixed32, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Kind)), Itab: shimlayout.ItabOf((*scratchOneofF)(nil)), Ogrp: 0, Sub: -1},
			{Fnum: 6, Kind: shimlayout.KindDouble, Wire: shimlayout.WireFixed64, Offset: uint32(unsafe.Offsetof(scratchOneof{}.Kind)), Itab: shimlayout.ItabOf((*scratchOneofD)(nil)), Ogrp: 0, Sub: -1},
		}},
		{Full: "scratch4.Inner", Size: unsafe.Sizeof(scratchOneofInner{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchOneofInner{}.V)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchOneofInner{}.W)), Sub: -1},
		}},
	}
}

func oracleOneof(t *testing.T, md protoreflect.MessageDescriptor, kind protoreflect.FieldNumber, v protoreflect.Value) []byte {
	t.Helper()
	m := dynamicpb.NewMessage(md)
	m.Set(md.Fields().ByNumber(1), protoreflect.ValueOfString("n"))
	if kind != 0 {
		m.Set(md.Fields().ByNumber(kind), v)
	}
	wire, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	return wire
}

// fixupOneof is what the generated model will own: wrap the staged
// occurrence into its wrapper with a typed store. Strings copy out of
// the wire; the message member reads its scalar from the C-arena
// submessage. Slot 0 is the test registry's only oneof group.
func fixupOneof(t *testing.T, c *Codec, m *scratchOneof, wire []byte) {
	t.Helper()
	fnum, _, scalar, ptr, ln, ok := c.Stage(0)
	if !ok {
		t.Fatal("stage read failed")
	}
	switch fnum {
	case 0:
		m.Kind = nil
	case 2:
		m.Kind = &scratchOneofN{N: int32(scalar)}
	case 3:
		m.Kind = &scratchOneofT{T: string(unsafe.Slice((*byte)(ptr), ln))}
	case 5:
		m.Kind = &scratchOneofF{F: *(*float32)(unsafe.Pointer(&scalar))}
	case 6:
		m.Kind = &scratchOneofD{D: *(*float64)(unsafe.Pointer(&scalar))}
	case 4:
		if ptr == nil {
			t.Fatal("message member staged nil submessage")
		}
		m.Kind = &scratchOneofW{In: &scratchOneofInner{
			V: *(*int32)(ptr),
			W: *(*int32)(unsafe.Add(ptr, 4)),
		}}
	default:
		t.Fatalf("unexpected staged fnum %d", fnum)
	}
	runtime.KeepAlive(wire)
}

// ---------------------------------------------------------------------------
// Maps ride as repeated entry structs: array order is the wire order (like
// the C struct binding), and message-valued entries nest one more level.
// The oracle is dynamicpb over a map field, marshalled deterministically.
// ---------------------------------------------------------------------------

type scratchMapEntry struct {
	Key string
	Val int32
}

type scratchMapMsgEntry struct {
	Key string
	In  *scratchOneofInner
}

type scratchMap struct {
	Name  string
	ByStr []*scratchMapEntry
	ByMsg []*scratchMapMsgEntry
}

func scratchMapDescriptor(t *testing.T) protoreflect.MessageDescriptor {
	t.Helper()
	str := func(s string) *string { return &s }
	i32 := func(n int32) *int32 { return &n }
	entry := func(name string, num int32, key, val *descriptorpb.FieldDescriptorProto) *descriptorpb.DescriptorProto {
		return &descriptorpb.DescriptorProto{
			Name:    str(name),
			Field:   []*descriptorpb.FieldDescriptorProto{key, val},
			Options: &descriptorpb.MessageOptions{MapEntry: func() *bool { b := true; return &b }()},
		}
	}
	sf := func(name string, num int32, typ descriptorpb.FieldDescriptorProto_Type) *descriptorpb.FieldDescriptorProto {
		return &descriptorpb.FieldDescriptorProto{
			Name: str(name), Number: i32(num), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: typ.Enum(),
		}
	}
	fd := &descriptorpb.FileDescriptorProto{
		Syntax:  str("proto3"),
		Name:    str("scratch5.proto"),
		Package: str("scratch5"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: str("Inner"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: str("v"), Number: i32(1), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
					{Name: str("w"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum()},
				},
			},
			{
				Name: str("S"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: str("name"), Number: i32(1), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum()},
					{Name: str("by_str"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_REPEATED.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: str(".scratch5.S.ByStrEntry")},
					{Name: str("by_msg"), Number: i32(3), Label: descriptorpb.FieldDescriptorProto_LABEL_REPEATED.Enum(), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: str(".scratch5.S.ByMsgEntry")},
				},
				NestedType: []*descriptorpb.DescriptorProto{
					entry("ByStrEntry", 2, sf("key", 1, descriptorpb.FieldDescriptorProto_TYPE_STRING), sf("value", 2, descriptorpb.FieldDescriptorProto_TYPE_INT32)),
					entry("ByMsgEntry", 3, sf("key", 1, descriptorpb.FieldDescriptorProto_TYPE_STRING), &descriptorpb.FieldDescriptorProto{
						Name: str("value"), Number: i32(2), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
						Type:     descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(),
						TypeName: str(".scratch5.Inner"),
					}),
				},
			},
		},
	}
	file, err := protodesc.NewFile(fd, nil)
	if err != nil {
		t.Fatal(err)
	}
	return file.Messages().ByName("S")
}

// Split message occurrences merge into the staged submessage; a different
// member arriving later still replaces the slot.
func TestCshimSplitOneofMerge(t *testing.T) {
	reg := scratchOneofRows()
	c, err := NewCodecFromLayout(reg, "scratch4.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	w1 := testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofW{In: &scratchOneofInner{V: 1}}})
	w2 := testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofW{In: &scratchOneofInner{W: 2}}})
	both := append(append([]byte{}, w1...), w2...)
	back := &scratchOneof{}
	testDecode(t, c, back, both)
	fixupOneof(t, c, back, both)
	if !reflect.DeepEqual(back.Kind, &scratchOneofW{In: &scratchOneofInner{V: 1, W: 2}}) {
		t.Fatalf("split merge gave %#v", back.Kind)
	}
	// Re-encoding the merged struct is the canonical single occurrence,
	// not the split input.
	wantMerged := testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofW{In: &scratchOneofInner{V: 1, W: 2}}})
	if again := testEncode(t, c, back); !bytes.Equal(again, wantMerged) {
		t.Fatalf("merged re-encode differs: %x vs %x", again, wantMerged)
	}

	// Different member later: replace, not merge.
	wt := testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofT{T: "hi"}})
	mix := append(append([]byte{}, w1...), wt...)
	backMix := &scratchOneof{}
	testDecode(t, c, backMix, mix)
	fixupOneof(t, c, backMix, mix)
	if !reflect.DeepEqual(backMix.Kind, &scratchOneofT{T: "hi"}) {
		t.Fatalf("replace gave %#v", backMix.Kind)
	}
}

// A sub table cannot carry repeated rows: decode publishes repeat buffers
// for the root table only, so a nested repeat would silently decode nothing.
// The root itself may repeat, and a table outside the root's closure is
// never walked, so its shape is not checked.
func TestCshimSubRepeatedRules(t *testing.T) {
	var r scratchG
	var sub scratchSubRep
	reg := []shimlayout.Msg{
		{Full: "scratch7.R", Size: unsafe.Sizeof(r), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(r.Grp)), Sub: 1},
		}},
		{Full: "scratch7.S", Size: unsafe.Sizeof(sub), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Repeated: true, Offset: uint32(unsafe.Offsetof(sub.Tags)), Sub: -1},
		}},
	}
	if _, err := NewCodecFromLayout(reg, "scratch7.R"); err == nil {
		t.Fatal("a repeated row in a sub table must be refused")
	}
	// The same table is fine as the root: the rule tracks the one table decode
	// publishes its repeat buffers for, not the row shape.
	c2, err := NewCodecFromLayout(reg, "scratch7.S")
	if err != nil {
		t.Fatalf("the root may repeat: %v", err)
	}
	c2.Close()

	var m scratchMap
	var unused scratchRep
	accepted := []shimlayout.Msg{
		{Full: "scratch8.R", Size: unsafe.Sizeof(m), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(m.Name)), Sub: -1},
		}},
		{Full: "scratch8.Unused", Size: unsafe.Sizeof(unused), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Repeated: true, Offset: uint32(unsafe.Offsetof(unused.V)), Sub: -1},
		}},
	}
	c, err := NewCodecFromLayout(accepted, "scratch8.R")
	if err != nil {
		t.Fatalf("an unreachable table must not be validated: %v", err)
	}
	defer c.Close()
	if got := testEncode(t, c, &scratchMap{Name: "n"}); !bytes.Equal(got, []byte{0x0a, 0x01, 'n'}) {
		t.Fatalf("encode differs: %x", got)
	}
}

// A row naming a message from another file cannot be walked: the codec's
// registry is this file's messages, so New must refuse the row instead of
// indexing the table array with Sub -1. The Go emitter keeps such a file on
// the pure-Go engine (TestGoCshimForeignFieldFallsBack), so only a
// hand-written layout reaches this guard.
func TestCshimForeignRowRefused(t *testing.T) {
	var s scratchG
	reg := []shimlayout.Msg{
		{Full: "scratch6.S", Size: unsafe.Sizeof(s), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(s.Grp)), Sub: -1, Foreign: true, FType: "other.Leaf"},
		}},
	}
	if _, err := NewCodecFromLayout(reg, "scratch6.S"); err == nil {
		t.Fatal("a foreign row must be refused at New time")
	}
}

// Unknown policy for the thin model: unknown field numbers skip and stay
// dropped on re-encode (the DMsg tree is the retention path for gateway
// use, mirroring TestConvertModelDropsUnknownOnExport). A known field
// carrying the wrong wire type fails the decode, like protobuf-go.
func TestCshimUnknownDropped(t *testing.T) {
	var s scratchMap
	reg := []shimlayout.Msg{
		{Full: "scratch5.S", Size: unsafe.Sizeof(s), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(s.Name)), Sub: -1},
		}},
	}
	c, err := NewCodecFromLayout(reg, "scratch5.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	// Unknown field 9 (varint 7) rides along and is dropped on re-encode.
	wire := []byte{0x0a, 0x01, 'n', 0x48, 0x07}
	back := &scratchMap{}
	testDecode(t, c, back, wire)
	runtime.KeepAlive(wire)
	if back.Name != "n" {
		t.Fatalf("known field lost: %q", back.Name)
	}
	if again := testEncode(t, c, back); !bytes.Equal(again, []byte{0x0a, 0x01, 'n'}) {
		t.Fatalf("unknown field survived: %x", again)
	}

	// Known field 1 sent as a varint must fail, like the oracle.
	if _, err := c.DecodeRaw(unsafe.Pointer(&scratchMap{}), []byte{0x08, 0x01}); err == nil {
		t.Fatal("wrong wire type decoded without error")
	}

	// An unknown legacy group skips whole, and an unterminated one fails.
	withGroup := []byte{0x0a, 0x01, 'n', 0x4b, 0x08, 0x01, 0x4c}
	gback := &scratchMap{}
	testDecode(t, c, gback, withGroup)
	if gback.Name != "n" {
		t.Fatalf("group skip lost the known field: %q", gback.Name)
	}
	if _, err := c.DecodeRaw(unsafe.Pointer(&scratchMap{}), []byte{0x4b, 0x08, 0x01}); err == nil {
		t.Fatal("unterminated group decoded without error")
	}
}

// Nested oneofs must not share staging slots with their root: each table
// owns its slots, keyed by (table, slot).
type scratchNestOuter struct {
	Name string
	In   *scratchNestInner
	A    any
}

type scratchNestInner struct {
	A any
}

func TestCshimNestedOneofSlots(t *testing.T) {
	var o scratchNestOuter
	var in scratchNestInner
	reg := []shimlayout.Msg{
		{Full: "nest.O", Size: unsafe.Sizeof(o), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(o.Name)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(o.In)), Sub: 1},
			{Fnum: 3, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(o.A)), Itab: shimlayout.ItabFor[any]((*scratchOneofT)(nil)), Ogrp: 0, Sub: -1},
		}},
		{Full: "nest.I", Size: unsafe.Sizeof(in), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(in.A)), Itab: shimlayout.ItabFor[any]((*scratchOneofT)(nil)), Ogrp: 0, Sub: -1},
		}},
	}
	c, err := NewCodecFromLayout(reg, "nest.O")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	// Root member 3 then nested member 1: both are slot 0 in their own
	// table, so the nested one must not clobber the root one.
	msg := &scratchNestOuter{Name: "root", In: &scratchNestInner{A: &scratchOneofT{T: "inner"}}, A: &scratchOneofT{T: "outer"}}
	wire := testEncode(t, c, msg)

	// Decode stages both members; the root fixup and the nested fixup each
	// read their own table's slot.
	back := &scratchNestOuter{}
	testDecode(t, c, back, wire)
	runtime.KeepAlive(wire)
	// Root member staged in the root table's range, nested member in the
	// nested table's: neither range overwrote the other.
	if fnum, _, _, _, ln, ok := c.Stage(0); !ok || fnum != 3 || ln != len("outer") {
		t.Fatalf("root slot: fnum=%d len=%d ok=%v", fnum, ln, ok)
	}
	if fnum, _, _, _, ln, ok := c.Stage(1); !ok || fnum != 1 || ln != len("inner") {
		t.Fatalf("nested slot: fnum=%d len=%d ok=%v", fnum, ln, ok)
	}
	if back.Name != "root" {
		t.Fatalf("root field lost: %q", back.Name)
	}
}

// Singular (non-oneof) message occurrences merge, like protobuf: a message
// split across the wire keeps every sub-field set by any occurrence.
func TestCshimSingularMessageMerge(t *testing.T) {
	reg := scratchGroupRows()
	c, err := NewCodecFromLayout(reg, "scratch3.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	// Field 3 is a singular Sub; field 1 of Sub is the singular Leaf.
	w1 := testEncode(t, c, &scratchG{Sub: &scratchSub{Leaf: &scratchLeaf{W: 1}}})
	w2 := testEncode(t, c, &scratchG{Sub: &scratchSub{N: 2}})
	both := append(append([]byte{}, w1...), w2...)
	back := &scratchG{}
	testDecode(t, c, back, both)
	runtime.KeepAlive(both)
	if back.Sub == nil || back.Sub.Leaf == nil || back.Sub.Leaf.W != 1 || back.Sub.N != 2 {
		t.Fatalf("singular merge gave %+v (leaf %+v)", back.Sub, back.Sub.Leaf)
	}
}

// One Codec is serialized by its own lock: concurrent Marshal/Unmarshal on
// the same codec return exactly what serial calls return. Run with -race to
// prove there is no data race in the C scratch or the encode size hint.
func TestCshimConcurrentUse(t *testing.T) {
	reg := scratchOneofRows()
	c, err := NewCodecFromLayout(reg, "scratch4.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	cases := []struct {
		want []byte
		kind any
	}{
		{testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofN{N: -7}}), &scratchOneofN{N: -7}},
		{testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofT{T: "hi"}}), &scratchOneofT{T: "hi"}},
		{testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofF{F: 2.5}}), &scratchOneofF{F: 2.5}},
	}
	var wg sync.WaitGroup
	errCh := make(chan string, 64)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for k := 0; k < 200; k++ {
				tc := cases[(i+k)%len(cases)]
				m := &scratchOneof{Name: "n", Kind: tc.kind}
				got, err := c.EncodeRaw(unsafe.Pointer(m))
				if err != nil {
					errCh <- "encode: " + err.Error()
					return
				}
				if !bytes.Equal(got, tc.want) {
					errCh <- fmt.Sprintf("encode differs: %x vs %x", got, tc.want)
					return
				}
				back := &scratchOneof{}
				if _, err := c.DecodeRaw(unsafe.Pointer(back), tc.want); err != nil {
					errCh <- "decode: " + err.Error()
					return
				}
			}
		}(i)
	}
	wg.Wait()
	close(errCh)
	for msg := range errCh {
		t.Fatal(msg)
	}
}

func TestCshimCodecMap(t *testing.T) {
	md := scratchMapDescriptor(t)
	var s scratchMap
	reg := []shimlayout.Msg{
		{Full: "scratch5.S", Size: unsafe.Sizeof(s), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(s.Name)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(s.ByStr)), Repeated: true, Sub: 1},
			{Fnum: 3, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(s.ByMsg)), Repeated: true, Sub: 2},
		}},
		{Full: "scratch5.S.ByStrEntry", Size: unsafe.Sizeof(scratchMapEntry{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchMapEntry{}.Key)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchMapEntry{}.Val)), Sub: -1},
		}},
		{Full: "scratch5.S.ByMsgEntry", Size: unsafe.Sizeof(scratchMapMsgEntry{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchMapMsgEntry{}.Key)), Sub: -1},
			{Fnum: 2, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(unsafe.Offsetof(scratchMapMsgEntry{}.In)), Sub: 3},
		}},
		{Full: "scratch5.Inner", Size: unsafe.Sizeof(scratchOneofInner{}), Rows: []shimlayout.Row{
			{Fnum: 1, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(unsafe.Offsetof(scratchOneofInner{}.V)), Sub: -1},
		}},
	}
	c, err := NewCodecFromLayout(reg, "scratch5.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	// Deterministic oracle: keys in sorted order, matching the array order.
	m := dynamicpb.NewMessage(md)
	m.Set(md.Fields().ByNumber(1), protoreflect.ValueOfString("n"))
	mp := m.Mutable(md.Fields().ByNumber(2)).Map()
	mp.Set(protoreflect.ValueOfString("a").MapKey(), protoreflect.ValueOfInt32(1))
	mp.Set(protoreflect.ValueOfString("b").MapKey(), protoreflect.ValueOfInt32(-2))
	mm := m.Mutable(md.Fields().ByNumber(3)).Map()
	innerMd := md.ParentFile().Messages().ByName("Inner")
	inner := dynamicpb.NewMessage(innerMd)
	inner.Set(innerMd.Fields().ByNumber(1), protoreflect.ValueOfInt32(9))
	mm.Set(protoreflect.ValueOfString("k").MapKey(), protoreflect.ValueOfMessage(inner))
	ref, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}

	in := &scratchMap{
		Name:  "n",
		ByStr: []*scratchMapEntry{{Key: "a", Val: 1}, {Key: "b", Val: -2}},
		ByMsg: []*scratchMapMsgEntry{{Key: "k", In: &scratchOneofInner{V: 9}}},
	}
	if got := testEncode(t, c, in); !bytes.Equal(got, ref) {
		t.Fatalf("encode differs: %x vs %x", got, ref)
	}
	back := &scratchMap{}
	testDecode(t, c, back, ref)
	runtime.KeepAlive(ref)
	if back.Name != "n" || len(back.ByStr) != 2 || back.ByStr[0].Key != "a" ||
		back.ByStr[0].Val != 1 || back.ByStr[1].Key != "b" || back.ByStr[1].Val != -2 {
		t.Fatalf("by_str mismatch: %+v", back.ByStr)
	}
	if len(back.ByMsg) != 1 || back.ByMsg[0].Key != "k" || back.ByMsg[0].In == nil ||
		back.ByMsg[0].In.V != 9 {
		t.Fatalf("by_msg mismatch: %+v", back.ByMsg)
	}
	if again := testEncode(t, c, back); !bytes.Equal(again, ref) {
		t.Fatalf("re-encode differs: %x vs %x", again, ref)
	}
}

func TestCshimCodecOneof(t *testing.T) {
	md := scratchOneofDescriptor(t)
	reg := scratchOneofRows()
	c, err := NewCodecFromLayout(reg, "scratch4.S")
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	// Zero-valued members still encode: presence lives in the wrapper.
	cases := []struct {
		name string
		kind any
		fnum protoreflect.FieldNumber
		val  protoreflect.Value
	}{
		{"zero-int", &scratchOneofN{}, 2, protoreflect.ValueOfInt32(0)},
		{"neg-int", &scratchOneofN{N: -7}, 2, protoreflect.ValueOfInt32(-7)},
		{"empty-string", &scratchOneofT{}, 3, protoreflect.ValueOfString("")},
		{"text", &scratchOneofT{T: "hi"}, 3, protoreflect.ValueOfString("hi")},
	}
	for _, tc := range cases {
		m := &scratchOneof{Name: "n", Kind: tc.kind}
		ref := oracleOneof(t, md, tc.fnum, tc.val)
		if got := testEncode(t, c, m); !bytes.Equal(got, ref) {
			t.Fatalf("%s: encode differs: %x vs %x", tc.name, got, ref)
		}
		rt := dynamicpb.NewMessage(md)
		if err := proto.Unmarshal(ref, rt); err != nil {
			t.Fatalf("%s: oracle reparse: %v", tc.name, err)
		}
		want := dynamicpb.NewMessage(md)
		want.Set(md.Fields().ByNumber(1), protoreflect.ValueOfString("n"))
		want.Set(md.Fields().ByNumber(tc.fnum), tc.val)
		if !proto.Equal(want, rt) {
			t.Fatalf("%s: oracle reparse mismatch", tc.name)
		}

		// Decode stages the member; the fixup wraps it, and
		// re-encoding the fixed-up struct is byte-stable.
		back := &scratchOneof{}
		testDecode(t, c, back, ref)
		if back.Name != "n" {
			t.Fatalf("%s: name lost: %q", tc.name, back.Name)
		}
		fixupOneof(t, c, back, ref)
		if !reflect.DeepEqual(back.Kind, tc.kind) {
			t.Fatalf("%s: fixup gave %#v, want %#v", tc.name, back.Kind, tc.kind)
		}
		if again := testEncode(t, c, back); !bytes.Equal(again, ref) {
			t.Fatalf("%s: re-encode differs: %x vs %x", tc.name, again, ref)
		}
	}

	// Message member round-trips through the C-arena submessage.
	innerMd := md.Fields().ByNumber(4).Message()
	im := dynamicpb.NewMessage(innerMd)
	im.Set(innerMd.Fields().ByNumber(1), protoreflect.ValueOfInt32(9))
	innerRef := oracleOneof(t, md, 4, protoreflect.ValueOfMessage(im))
	innerKind := &scratchOneofW{In: &scratchOneofInner{V: 9}}
	if got := testEncode(t, c, &scratchOneof{Name: "n", Kind: innerKind}); !bytes.Equal(got, innerRef) {
		t.Fatalf("message: encode differs: %x vs %x", got, innerRef)
	}
	backInner := &scratchOneof{}
	testDecode(t, c, backInner, innerRef)
	fixupOneof(t, c, backInner, innerRef)
	if !reflect.DeepEqual(backInner.Kind, innerKind) {
		t.Fatalf("message: fixup gave %#v, want %#v", backInner.Kind, innerKind)
	}
	if again := testEncode(t, c, backInner); !bytes.Equal(again, innerRef) {
		t.Fatalf("message: re-encode differs: %x vs %x", again, innerRef)
	}

	// Typed-nil message member encodes the empty message, matching the
	// oracle; decode normalizes to a non-nil empty inner.
	nilRef := oracleOneof(t, md, 4, protoreflect.ValueOfMessage(dynamicpb.NewMessage(innerMd)))
	if got := testEncode(t, c, &scratchOneof{Name: "n", Kind: &scratchOneofW{}}); !bytes.Equal(got, nilRef) {
		t.Fatalf("typed-nil: encode differs: %x vs %x", got, nilRef)
	}
	backNil := &scratchOneof{}
	testDecode(t, c, backNil, nilRef)
	fixupOneof(t, c, backNil, nilRef)
	if !reflect.DeepEqual(backNil.Kind, &scratchOneofW{In: &scratchOneofInner{}}) {
		t.Fatalf("typed-nil: fixup gave %#v", backNil.Kind)
	}
	if again := testEncode(t, c, backInner); !bytes.Equal(again, innerRef) {
		t.Fatalf("message: re-encode differs: %x vs %x", again, innerRef)
	}

	// Unset oneof encodes only the name field and fixes up to nil.
	bareRef := oracleOneof(t, md, 0, protoreflect.Value{})
	if got := testEncode(t, c, &scratchOneof{Name: "n"}); !bytes.Equal(got, bareRef) {
		t.Fatalf("unset: encode differs: %x vs %x", got, bareRef)
	}
	backBare := &scratchOneof{}
	testDecode(t, c, backBare, bareRef)
	fixupOneof(t, c, backBare, bareRef)
	if backBare.Kind != nil {
		t.Fatalf("unset: fixup gave %#v, want nil", backBare.Kind)
	}

	// Fixed-width members: encode reads the wrapper payload, not the
	// interface header (a header read would emit the itab word).
	for _, tc := range []struct {
		name string
		kind any
		fnum protoreflect.FieldNumber
		val  protoreflect.Value
	}{
		{"float", &scratchOneofF{F: 2.5}, 5, protoreflect.ValueOfFloat32(2.5)},
		{"double", &scratchOneofD{D: -0.5}, 6, protoreflect.ValueOfFloat64(-0.5)},
		{"float-zero", &scratchOneofF{}, 5, protoreflect.ValueOfFloat32(0)},
	} {
		ref := oracleOneof(t, md, tc.fnum, tc.val)
		if got := testEncode(t, c, &scratchOneof{Name: "n", Kind: tc.kind}); !bytes.Equal(got, ref) {
			t.Fatalf("%s: encode differs: %x vs %x", tc.name, got, ref)
		}
		backFix := &scratchOneof{}
		testDecode(t, c, backFix, ref)
		fixupOneof(t, c, backFix, ref)
		if !reflect.DeepEqual(backFix.Kind, tc.kind) {
			t.Fatalf("%s: fixup gave %#v, want %#v", tc.name, backFix.Kind, tc.kind)
		}
	}

	// Last occurrence wins across members sharing the group slot.
	// Duplicate scalar fields are valid wire; the parser keeps the last.
	first := oracleOneof(t, md, 2, protoreflect.ValueOfInt32(-7))
	second := oracleOneof(t, md, 3, protoreflect.ValueOfString("hi"))
	both := append(append([]byte{}, first...), second...)
	backLast := &scratchOneof{}
	testDecode(t, c, backLast, both)
	fixupOneof(t, c, backLast, both)
	if !reflect.DeepEqual(backLast.Kind, &scratchOneofT{T: "hi"}) {
		t.Fatalf("last-wins: fixup gave %#v", backLast.Kind)
	}
}
