// Dynamic-core branches the matrix3/oneof1 fixtures cannot reach: oneof
// clearing before the first encode, split oneof-message merging, field-number
// ordering, repeated legacy groups, and presence after decode.
package runtime_test

import (
	"bytes"
	"os"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"pbwire/runtime"
)

func loadBranches(t *testing.T) *runtime.DType {
	t.Helper()
	raw, err := os.ReadFile("testdata/branches.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ, err := s.Type("branches.Branches")
	if err != nil {
		t.Fatal(err)
	}
	return typ
}

// Setting two oneof members before the first encode must clear the first: the
// sibling list may not depend on a table having been built already.
func TestDMsgOneofClearsBeforeEncode(t *testing.T) {
	raw, err := os.ReadFile("testdata/oneof1.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ, err := s.Type("spectest.Oneof1")
	if err != nil {
		t.Fatal(err)
	}
	m := typ.New() // no encode/decode has built the table yet
	if err := m.SetInt("num", 5); err != nil {
		t.Fatal(err)
	}
	if err := m.SetString("text", "t"); err != nil {
		t.Fatal(err)
	}
	wire, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if want := []byte{0x1a, 0x01, 't'}; !bytes.Equal(wire, want) {
		t.Fatalf("encode % x, want % x (only the winner)", wire, want)
	}
}

// Two occurrences of the same oneof message member merge, like the reference
// runtime: the second occurrence carries b, the first carried a.
func TestDMsgSplitOneofMessageMerges(t *testing.T) {
	typ := loadBranches(t)
	wire := []byte{0x0a, 0x02, 0x08, 0x05, 0x0a, 0x02, 0x10, 0x07}
	m, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	sub, ok, err := m.GetMessage("sub")
	if err != nil || !ok || sub == nil {
		t.Fatalf("sub: ok=%v err=%v", ok, err)
	}
	if v, _, err := sub.GetInt("a"); err != nil || v != 5 {
		t.Fatalf("a = %d (%v), want 5", v, err)
	}
	if v, _, err := sub.GetInt("b"); err != nil || v != 7 {
		t.Fatalf("b = %d (%v), want 7", v, err)
	}
	out, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if want := []byte{0x0a, 0x04, 0x08, 0x05, 0x10, 0x07}; !bytes.Equal(out, want) {
		t.Fatalf("encode % x, want merged % x", out, want)
	}
}

// A split oneof message member merges recursively: scalar fields take the
// last value and singular submessages merge instead of being replaced, like
// Message::MergeFrom in the reference runtime.
func TestDMsgSplitOneofNestedMerges(t *testing.T) {
	leaf := &descriptorpb.DescriptorProto{
		Name: jstrp("Leaf"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("x", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			jfield("y", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
		},
	}
	mid := &descriptorpb.DescriptorProto{
		Name: jstrp("Mid"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("a", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			jfield("leaf", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".nesttest.Leaf"),
		},
	}
	top := &descriptorpb.DescriptorProto{
		Name: jstrp("Top"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("mid", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".nesttest.Mid"),
		},
		OneofDecl: []*descriptorpb.OneofDescriptorProto{{Name: jstrp("kind")}},
	}
	top.Field[0].OneofIndex = proto.Int32(0)
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{
		{
			Name:        jstrp("nest.proto"),
			Package:     jstrp("nesttest"),
			Syntax:      jstrp("proto3"),
			MessageType: []*descriptorpb.DescriptorProto{leaf, mid, top},
		},
	}})
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ, err := s.Type("nesttest.Top")
	if err != nil {
		t.Fatal(err)
	}
	// mid{a:1 leaf{x:1}} then mid{leaf{y:2}}: the winner keeps a and x
	// from the first occurrence and gains y from the second.
	wire := []byte{0x0a, 0x06, 0x08, 0x01, 0x12, 0x02, 0x08, 0x01,
		0x0a, 0x04, 0x12, 0x02, 0x10, 0x02}
	m, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	sub, ok, err := m.GetMessage("mid")
	if err != nil || !ok || sub == nil {
		t.Fatalf("mid: ok=%v err=%v", ok, err)
	}
	if v, _, err := sub.GetInt("a"); err != nil || v != 1 {
		t.Fatalf("a = %d (%v), want 1", v, err)
	}
	inner, ok, err := sub.GetMessage("leaf")
	if err != nil || !ok || inner == nil {
		t.Fatalf("leaf: ok=%v err=%v", ok, err)
	}
	if v, _, err := inner.GetInt("x"); err != nil || v != 1 {
		t.Fatalf("x = %d (%v), want 1", v, err)
	}
	if v, _, err := inner.GetInt("y"); err != nil || v != 2 {
		t.Fatalf("y = %d (%v), want 2", v, err)
	}
}

// Fields declared out of order still encode in field-number order.
func TestDMsgFieldNumberOrder(t *testing.T) {
	typ := loadBranches(t)
	m := typ.New()
	if err := m.SetInt("one", 1); err != nil {
		t.Fatal(err)
	}
	if err := m.SetInt("two", 2); err != nil {
		t.Fatal(err)
	}
	wire, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if want := []byte{0x18, 0x01, 0x20, 0x02}; !bytes.Equal(wire, want) {
		t.Fatalf("encode % x, want number-ordered % x", wire, want)
	}
}

// Repeated legacy groups round-trip with start/end framing, not length prefixes.
func TestDMsgRepeatedGroupFraming(t *testing.T) {
	typ := loadBranches(t)
	wire := []byte{0x13, 0x08, 0x01, 0x14, 0x13, 0x08, 0x02, 0x14}
	m, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	items, err := m.GetMessageSlice("item")
	if err != nil || len(items) != 2 {
		t.Fatalf("items = %d (%v)", len(items), err)
	}
	out, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(out, wire) {
		t.Fatalf("re-encode % x, want % x", out, wire)
	}
	built := typ.New()
	for _, v := range []int64{1, 2} {
		item, err := built.NewMessage("item")
		if err != nil {
			t.Fatal(err)
		}
		if err := item.SetInt("v", v); err != nil {
			t.Fatal(err)
		}
	}
	bwire, err := built.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(bwire, wire) {
		t.Fatalf("built % x, want % x", bwire, wire)
	}
}

// A decoded child is present: Has must not report an absent message when the
// bytes carried one.
func TestDMsgHasAfterDecode(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	ref, err := fillScalarsStruct(1).Marshal()
	if err != nil {
		t.Fatal(err)
	}
	m, err := typ.Decode(ref)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"inner", "inners"} {
		if ok, err := m.Has(name); err != nil || !ok {
			t.Fatalf("Has(%q) = %v (%v), want true", name, ok, err)
		}
	}
}
