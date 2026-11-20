// Plan tests: the schema plan for the real matrix3/oneof1/maptest
// descriptor sets matches the .proto sources field by field.
package codec_test

import (
	"os"
	"testing"

	"google.golang.org/protobuf/types/descriptorpb"
	"pbwire/codec"
)

func mustLoad(t *testing.T, path string) *codec.Schema {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	s, err := codec.Load(raw)
	if err != nil {
		t.Fatalf("Load(%s): %v", path, err)
	}
	return s
}

// Every Scalars3 field: number, proto type, wire type, presence, packing.
// This table pins the schema derivation the straight-line model below (and
// the future codegen emitter) programs against.
func TestPlanScalars3(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	m, err := s.Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	T := descriptorpb.FieldDescriptorProto_TYPE_INT32
	_ = T
	type want struct {
		name     string
		num      uint32
		proto    descriptorpb.FieldDescriptorProto_Type
		wire     codec.WireType
		presence codec.Presence
		packed   bool
		repeat   bool
	}
	INT32 := descriptorpb.FieldDescriptorProto_TYPE_INT32
	INT64 := descriptorpb.FieldDescriptorProto_TYPE_INT64
	UINT32 := descriptorpb.FieldDescriptorProto_TYPE_UINT32
	UINT64 := descriptorpb.FieldDescriptorProto_TYPE_UINT64
	SINT32 := descriptorpb.FieldDescriptorProto_TYPE_SINT32
	SINT64 := descriptorpb.FieldDescriptorProto_TYPE_SINT64
	FIXED32 := descriptorpb.FieldDescriptorProto_TYPE_FIXED32
	FIXED64 := descriptorpb.FieldDescriptorProto_TYPE_FIXED64
	SFIXED32 := descriptorpb.FieldDescriptorProto_TYPE_SFIXED32
	SFIXED64 := descriptorpb.FieldDescriptorProto_TYPE_SFIXED64
	FLOAT := descriptorpb.FieldDescriptorProto_TYPE_FLOAT
	DOUBLE := descriptorpb.FieldDescriptorProto_TYPE_DOUBLE
	BOOL := descriptorpb.FieldDescriptorProto_TYPE_BOOL
	STRING := descriptorpb.FieldDescriptorProto_TYPE_STRING
	ENUM := descriptorpb.FieldDescriptorProto_TYPE_ENUM
	MESSAGE := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE
	V := codec.WireVarint
	F32 := codec.WireFixed32
	F64 := codec.WireFixed64
	L := codec.WireLen
	IMP := codec.PresenceImplicit
	OPT := codec.PresenceProto3Optional
	cases := []want{
		{"i32", 1, INT32, V, IMP, false, false},
		{"i64", 2, INT64, V, IMP, false, false},
		{"u32", 3, UINT32, V, IMP, false, false},
		{"u64", 4, UINT64, V, IMP, false, false},
		{"s32", 5, SINT32, V, IMP, false, false},
		{"s64", 6, SINT64, V, IMP, false, false},
		{"fx32", 7, FIXED32, F32, IMP, false, false},
		{"fx64", 8, FIXED64, F64, IMP, false, false},
		{"sfx32", 9, SFIXED32, F32, IMP, false, false},
		{"sfx64", 10, SFIXED64, F64, IMP, false, false},
		{"f32", 11, FLOAT, F32, IMP, false, false},
		{"f64", 12, DOUBLE, F64, IMP, false, false},
		{"flag", 13, BOOL, V, IMP, false, false},
		{"str", 14, STRING, L, IMP, false, false},
		{"color", 15, ENUM, V, IMP, false, false},
		{"inner", 16, MESSAGE, L, codec.PresenceExplicit, false, false},
		{"nums", 17, INT32, V, IMP, true, true},
		{"tags", 18, STRING, L, IMP, false, true},
		{"inners", 19, MESSAGE, L, IMP, false, true},
		{"opt_i32", 20, INT32, V, OPT, false, false},
		{"opt_str", 21, STRING, L, OPT, false, false},
		{"flags", 22, BOOL, V, IMP, true, true},
	}
	if len(m.Fields) != len(cases) {
		t.Fatalf("Scalars3 has %d fields, want %d", len(m.Fields), len(cases))
	}
	for i, c := range cases {
		fp, ok := m.ByNumber(c.num)
		if !ok {
			t.Fatalf("field %d (%s) missing from plan", c.num, c.name)
		}
		if m.Fields[i] != fp {
			t.Errorf("field %d (%s): declaration order broken", c.num, c.name)
		}
		if fp.Name != c.name || fp.Proto != c.proto || fp.Wire != c.wire ||
			fp.Presence != c.presence || fp.Packed != c.packed || fp.Repeated != c.repeat {
			t.Errorf("field %d: got %+v, want name=%s proto=%v wire=%v presence=%v packed=%v repeat=%v",
				c.num, fp, c.name, c.proto, c.wire, c.presence, c.packed, c.repeat)
		}
		if fn, ok := m.ByName(c.name); !ok || fn != fp {
			t.Errorf("field %s: ByName lookup mismatch", c.name)
		}
	}
	inner, ok := m.ByName("inner")
	if !ok || inner.Message != "structtest.Inner3" {
		t.Errorf("inner message ref = %q, want structtest.Inner3", inner.Message)
	}
	color, ok := m.ByName("color")
	if !ok || color.Enum != "structtest.Color3" {
		t.Errorf("color enum ref = %q, want structtest.Color3", color.Enum)
	}
	if len(m.Oneofs) != 0 {
		t.Errorf("Scalars3 oneofs = %v, want none", m.Oneofs)
	}

	in, err := s.Message("structtest.Inner3")
	if err != nil {
		t.Fatal(err)
	}
	if len(in.Fields) != 2 {
		t.Fatalf("Inner3 has %d fields, want 2", len(in.Fields))
	}
	name, _ := in.ByNumber(1)
	if name.Name != "name" || name.Proto != STRING || name.Wire != L || name.Presence != IMP {
		t.Errorf("Inner3.name = %+v", name)
	}
	val, _ := in.ByNumber(2)
	if val.Name != "value" || val.Proto != INT32 || val.Wire != V || val.Presence != IMP {
		t.Errorf("Inner3.value = %+v", val)
	}

	if _, err := s.Message("structtest.Nope"); err == nil {
		t.Error("Message(unknown) should fail")
	}
	if _, err := codec.Load([]byte{0xff, 0xff}); err == nil {
		t.Error("Load(garbage) should fail")
	}
}

func TestPlanOneof(t *testing.T) {
	s := mustLoad(t, "testdata/oneof1.desc")
	m, err := s.Message("spectest.Oneof1")
	if err != nil {
		t.Fatal(err)
	}
	if len(m.Oneofs) != 1 || m.Oneofs[0] != "kind" {
		t.Fatalf("oneofs = %v, want [kind]", m.Oneofs)
	}
	for _, tc := range []struct {
		name string
		num  uint32
	}{
		{"num", 2}, {"text", 3}, {"inner", 4},
	} {
		fp, ok := m.ByNumber(tc.num)
		if !ok {
			t.Fatalf("field %d missing", tc.num)
		}
		if fp.Name != tc.name || fp.Presence != codec.PresenceOneof || fp.Oneof != "kind" {
			t.Errorf("field %d = %+v, want oneof member kind/%s", tc.num, fp, tc.name)
		}
	}
	name, _ := m.ByNumber(1)
	if name.Presence != codec.PresenceImplicit || name.Oneof != "" {
		t.Errorf("name = %+v, want implicit non-oneof", name)
	}
	inner, _ := m.ByNumber(4)
	if inner.Message != "spectest.Oneof1.Inner" {
		t.Errorf("inner ref = %q", inner.Message)
	}
}

func TestPlanMap(t *testing.T) {
	s := mustLoad(t, "testdata/maptest.desc")
	m, err := s.Message("spectest.MapTest")
	if err != nil {
		t.Fatal(err)
	}
	INT32 := descriptorpb.FieldDescriptorProto_TYPE_INT32
	STRING := descriptorpb.FieldDescriptorProto_TYPE_STRING
	MESSAGE := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE
	byInt, _ := m.ByNumber(1)
	if !byInt.IsMap || !byInt.Repeated || byInt.Packed || byInt.MapKey != INT32 || byInt.MapValue != STRING {
		t.Errorf("by_int = %+v", byInt)
	}
	byStr, _ := m.ByNumber(2)
	if !byStr.IsMap || byStr.MapKey != STRING || byStr.MapValue != INT32 {
		t.Errorf("by_str = %+v", byStr)
	}
	byMsg, _ := m.ByNumber(3)
	if !byMsg.IsMap || byMsg.MapKey != STRING || byMsg.MapValue != MESSAGE ||
		byMsg.MapValueMsg != "spectest.MapValue" {
		t.Errorf("by_msg = %+v", byMsg)
	}
	// Entries stay addressable but flagged.
	entry, err := s.Message("spectest.MapTest.ByIntEntry")
	if err != nil {
		t.Fatal(err)
	}
	if !entry.IsMapEntry || len(entry.Fields) != 2 {
		t.Errorf("ByIntEntry = %+v", entry)
	}
}
