package runtime_test

// Regression tests for the runtime hardening pass: each one pins a case that
// used to produce wrong bytes, a wrong value, a panic or an unrecoverable
// stack overflow.

import (
	"bytes"
	"errors"
	"math"
	"os"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"pbwire/runtime"
	"pbwire/tests/gen/maptest"
	"pbwire/tests/gen/matrix3"
)

func loadSet(t *testing.T, files ...*descriptorpb.FileDescriptorProto) *runtime.Schema {
	t.Helper()
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: files})
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	return s
}

func msgOf(name string, opts *descriptorpb.MessageOptions, fields ...*descriptorpb.FieldDescriptorProto) *descriptorpb.DescriptorProto {
	return &descriptorpb.DescriptorProto{Name: jstrp(name), Options: opts, Field: fields}
}

func fileFeatures(fs *descriptorpb.FeatureSet) *descriptorpb.FileOptions {
	return &descriptorpb.FileOptions{Features: fs}
}

// A proto2 descriptor set omits the syntax field (protoc only spells
// "proto3"/"editions"). Reading it as proto3 dropped explicit presence and
// packed the repeated numerics.
func TestProto2WithoutSyntaxField(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("p2.proto"),
		Package: jstrp("p2"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("M", nil,
			jfield("a", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			jfield("nums", 2, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))},
	})
	typ := mustType(t, s, "p2.M")
	m := typ.New()
	mustSet(t, m.SetInt("a", 0))
	want := []byte{0x08, 0x00}
	if out, err := m.Encode(); err != nil || !bytes.Equal(out, want) {
		t.Fatalf("proto2 optional zero: %x (%v), want %x", out, err, want)
	}
	// Repeated numerics stay expanded: proto2 packs only on packed=true.
	back, err := typ.Decode([]byte{0x10, 0x01, 0x10, 0x02})
	mustSet(t, err)
	again, err := back.Encode()
	mustSet(t, err)
	if !bytes.Equal(again, []byte{0x10, 0x01, 0x10, 0x02}) {
		t.Fatalf("proto2 repeated numerics re-encoded packed: %x", again)
	}
}

// Edition 2023 defaults: explicit presence, packed repeats, verified UTF-8,
// open enums; a FeatureSet at file, message or field scope overrides them.
func TestEditionsFeatureSet(t *testing.T) {
	implicit := &descriptorpb.MessageOptions{
		Features: &descriptorpb.FeatureSet{FieldPresence: descriptorpb.FeatureSet_IMPLICIT.Enum()},
	}
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("e23.proto"),
		Package: jstrp("e23"),
		Syntax:  jstrp("editions"),
		MessageType: []*descriptorpb.DescriptorProto{
			msgOf("Defaults", nil,
				jfield("a", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
				jfield("nums", 2, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
				jfield("s", 3, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_STRING, "")),
			msgOf("Implicit", implicit,
				jfield("a", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
		},
	})

	def := mustType(t, s, "e23.Defaults")
	dm := def.New()
	mustSet(t, dm.SetInt("a", 0))
	out, err := dm.Encode()
	mustSet(t, err)
	if !bytes.Equal(out, []byte{0x08, 0x00}) {
		t.Fatalf("editions default explicit presence dropped a set zero: %x", out)
	}
	packed, err := def.Decode([]byte{0x10, 0x01, 0x10, 0x02})
	mustSet(t, err)
	again, err := packed.Encode()
	mustSet(t, err)
	if !bytes.Equal(again, []byte{0x12, 0x02, 0x01, 0x02}) {
		t.Fatalf("editions repeated numerics not packed: %x", again)
	}
	if _, err := def.Decode([]byte{0x1a, 0x01, 0xff}); !errors.Is(err, runtime.ErrUTF8) {
		t.Fatalf("editions string accepted invalid UTF-8: %v", err)
	}

	imp := mustType(t, s, "e23.Implicit")
	im := imp.New()
	mustSet(t, im.SetInt("a", 0))
	out, err = im.Encode()
	mustSet(t, err)
	if len(out) != 0 {
		t.Fatalf("message-level IMPLICIT presence emitted a zero: %x", out)
	}

	// Field-level overrides: EXPANDED repeats and NONE utf8 validation.
	fieldFile := &descriptorpb.FileDescriptorProto{
		Name:    jstrp("e23c.proto"),
		Package: jstrp("e23c"),
		Syntax:  jstrp("editions"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("F", nil,
			&descriptorpb.FieldDescriptorProto{
				Name: jstrp("nums"), Number: ji32p(1), Label: labp(jlabelRep),
				Type:    typp(descriptorpb.FieldDescriptorProto_TYPE_INT32),
				Options: &descriptorpb.FieldOptions{Features: &descriptorpb.FeatureSet{RepeatedFieldEncoding: descriptorpb.FeatureSet_EXPANDED.Enum()}},
			},
			&descriptorpb.FieldDescriptorProto{
				Name: jstrp("s"), Number: ji32p(2), Label: labp(jlabelOpt),
				Type:    typp(descriptorpb.FieldDescriptorProto_TYPE_STRING),
				Options: &descriptorpb.FieldOptions{Features: &descriptorpb.FeatureSet{Utf8Validation: descriptorpb.FeatureSet_NONE.Enum()}},
			})},
	}
	fs := loadSet(t, fieldFile)
	ft := mustType(t, fs, "e23c.F")
	fm, err := ft.Decode([]byte{0x08, 0x01, 0x08, 0x02})
	mustSet(t, err)
	out, err = fm.Encode()
	mustSet(t, err)
	if !bytes.Equal(out, []byte{0x08, 0x01, 0x08, 0x02}) {
		t.Fatalf("field-level EXPANDED not honored: %x", out)
	}
	if _, err := ft.Decode([]byte{0x12, 0x01, 0xff}); err != nil {
		t.Fatalf("field-level utf8 NONE rejected invalid UTF-8: %v", err)
	}

	// File-level enum_type CLOSED closes every enum declared in the file.
	closed := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("e23d.proto"),
		Package:     jstrp("e23d"),
		Syntax:      jstrp("editions"),
		Options:     fileFeatures(&descriptorpb.FeatureSet{EnumType: descriptorpb.FeatureSet_CLOSED.Enum()}),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("M", nil, jfield("e", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_ENUM, ".e23d.E"))},
		EnumType: []*descriptorpb.EnumDescriptorProto{{
			Name:  jstrp("E"),
			Value: []*descriptorpb.EnumValueDescriptorProto{{Name: jstrp("E_ZERO"), Number: ji32p(0)}},
		}},
	})
	ct := mustType(t, closed, "e23d.M")
	cm, err := ct.UnmarshalJSONString(`{"e":7}`, false)
	mustSet(t, err)
	out2, err := ct.MarshalJSONString(cm)
	mustSet(t, err)
	if out2 != `{}` {
		t.Fatalf("closed enum printed an unknown number: %s", out2)
	}
	open := mustType(t, s, "e23.Defaults")
	om, err := open.UnmarshalJSONString(`{"a":7}`, false)
	mustSet(t, err)
	_ = om
}

// A DELIMITED message field travels with group tags but keeps its field
// name and message typing.
func TestEditionsDelimitedMessageField(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("delim.proto"),
		Package: jstrp("delim"),
		Syntax:  jstrp("editions"),
		MessageType: []*descriptorpb.DescriptorProto{
			msgOf("Sub", nil, jfield("v", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
			msgOf("M", nil, &descriptorpb.FieldDescriptorProto{
				Name: jstrp("data"), Number: ji32p(1), Label: labp(jlabelOpt),
				Type:     typp(descriptorpb.FieldDescriptorProto_TYPE_MESSAGE),
				TypeName: jstrp(".delim.Sub"),
				Options:  &descriptorpb.FieldOptions{Features: &descriptorpb.FeatureSet{MessageEncoding: descriptorpb.FeatureSet_DELIMITED.Enum()}},
			}),
		},
	})
	typ := mustType(t, s, "delim.M")
	m := typ.New()
	sub, err := m.NewMessage("data")
	mustSet(t, err)
	mustSet(t, sub.SetInt("v", 1))
	out, err := m.Encode()
	mustSet(t, err)
	// field 1 start-group (0x0b), v=1, end-group (0x0c).
	if want := []byte{0x0b, 0x08, 0x01, 0x0c}; !bytes.Equal(out, want) {
		t.Fatalf("delimited message encoded %x, want %x", out, want)
	}
	back, err := typ.Decode(out)
	mustSet(t, err)
	again, err := back.Encode()
	mustSet(t, err)
	if !bytes.Equal(again, out) {
		t.Fatalf("delimited round trip changed bytes: %x vs %x", again, out)
	}
}

// Proto2 groups recurse through the depth cap instead of growing the stack.
func TestGroupDepthCap(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("g.proto"),
		Package: jstrp("g"),
		Syntax:  jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("G", nil,
			jfield("inner", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_GROUP, ".g.G"))},
	})
	typ := mustType(t, s, "g.G")
	var wire []byte
	for i := 0; i < 300; i++ {
		wire = append(wire, 0x0b)
	}
	for i := 0; i < 300; i++ {
		wire = append(wire, 0x0c)
	}
	if _, err := typ.DecodeLimits(wire, runtime.Limits{MaxDepth: 5}); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300 nested groups decoded under MaxDepth 5: %v", err)
	}
	if _, err := typ.Decode(wire); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300 nested groups decoded under the default cap: %v", err)
	}
	deep := typ.New()
	cur := deep
	for i := 0; i < 300; i++ {
		next, err := cur.NewMessage("inner")
		mustSet(t, err)
		cur = next
	}
	if _, err := deep.Encode(); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300-deep group tree encoded: %v", err)
	}
}

// NewMessage on a oneof member returns the child the tree keeps, so writes
// through it are not lost when a second call merges into the first.
func TestNewMessageOneofKeepsChild(t *testing.T) {
	raw, err := os.ReadFile("testdata/branches.desc")
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "branches.Branches")
	m := typ.New()
	c1, err := m.NewMessage("sub")
	mustSet(t, err)
	mustSet(t, c1.SetInt("a", 1))
	c2, err := m.NewMessage("sub")
	mustSet(t, err)
	mustSet(t, c2.SetInt("b", 2))
	if kept, ok, err := m.GetMessage("sub"); err != nil || !ok || kept != c2 {
		t.Fatalf("GetMessage = %p (%v), want the second child %p", kept, err, c2)
	}
	out, err := m.Encode()
	mustSet(t, err)
	if want := []byte{0x0a, 0x04, 0x08, 0x01, 0x10, 0x02}; !bytes.Equal(out, want) {
		t.Fatalf("oneof member writes lost: %x, want %x", out, want)
	}
}

// Two occurrences of the same oneof message field merge; a oneof member
// inside the merged child replaces the earlier member instead of keeping it.
func TestMergeOneofInsideSubmessage(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("m.proto"),
		Package: jstrp("m"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: jstrp("Leaf"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: jstrp("x"), Number: ji32p(1), Label: labp(jlabelOpt), Type: typp(descriptorpb.FieldDescriptorProto_TYPE_INT32), OneofIndex: proto.Int32(0)},
					{Name: jstrp("y"), Number: ji32p(2), Label: labp(jlabelOpt), Type: typp(descriptorpb.FieldDescriptorProto_TYPE_INT32), OneofIndex: proto.Int32(0)},
				},
				OneofDecl: []*descriptorpb.OneofDescriptorProto{{Name: jstrp("k")}},
			},
			{
				Name: jstrp("Outer"),
				Field: []*descriptorpb.FieldDescriptorProto{
					{Name: jstrp("mid"), Number: ji32p(1), Label: labp(jlabelOpt), Type: typp(descriptorpb.FieldDescriptorProto_TYPE_MESSAGE), TypeName: jstrp(".m.Leaf"), OneofIndex: proto.Int32(0)},
				},
				OneofDecl: []*descriptorpb.OneofDescriptorProto{{Name: jstrp("kind")}},
			},
		},
	})
	typ := mustType(t, s, "m.Outer")
	m, err := typ.Decode([]byte{0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x10, 0x02})
	mustSet(t, err)
	out, err := m.Encode()
	mustSet(t, err)
	// The newer occurrence set y, which clears x in the shared child.
	if want := []byte{0x0a, 0x02, 0x10, 0x02}; !bytes.Equal(out, want) {
		t.Fatalf("oneof member inside a merged child kept stale x: %x, want %x", out, want)
	}
}

// ImportModel keeps a negative zero: it is not the default value.
func TestImportModelNegativeZero(t *testing.T) {
	raw, err := os.ReadFile("testdata/matrix3.desc")
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "structtest.Scalars3")
	src := &matrix3.Scalars3{F32: float32(math.Copysign(0, -1)), F64: math.Copysign(0, -1)}
	want, err := runtime.EncodeModel(src, matrix3.Scalars3Meta)
	mustSet(t, err)
	tree, err := typ.ImportModel(src, matrix3.Scalars3Meta)
	mustSet(t, err)
	out, err := tree.Encode()
	mustSet(t, err)
	if !bytes.Equal(out, want) {
		t.Fatalf("ImportModel dropped -0: %x, want %x", out, want)
	}
}

// Deterministic encode sorts map entries; a nil element sorts as a missing
// key instead of panicking.
func TestDeterministicNilMapElement(t *testing.T) {
	mdl := &maptest.MapTest{ByStr: []*maptest.MapTest_ByStrEntry{nil, {Key: "a"}}}
	out, err := runtime.EncodeModelDeterministic(mdl, maptest.MapTestMeta)
	mustSet(t, err)
	if len(out) == 0 {
		t.Fatal("deterministic encode of a nil map element wrote nothing")
	}
}

// EncodeDeterministic uses the tree's own type, so a tree of another type
// cannot index past its slots.
func TestEncodeDeterministicUsesTreeType(t *testing.T) {
	raw, err := os.ReadFile("testdata/matrix3.desc")
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	scalars := mustType(t, s, "structtest.Scalars3")
	inner := mustType(t, s, "structtest.Inner3")
	tree := scalars.New()
	mustSet(t, tree.SetInt("i32", 7))
	plain, err := tree.Encode()
	mustSet(t, err)
	out, err := inner.EncodeDeterministic(tree)
	mustSet(t, err)
	if !bytes.Equal(out, plain) {
		t.Fatalf("deterministic encode of a foreign tree: %x, want %x", out, plain)
	}
}

// EncodeDelimited reports a body that cannot encode instead of framing an
// empty message.
func TestEncodeDelimitedReportsError(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("n.proto"),
		Package: jstrp("n"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("Node", nil,
			jfield("child", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".n.Node"))},
	})
	typ := mustType(t, s, "n.Node")
	root := typ.New()
	cur := root
	for i := 0; i < 300; i++ {
		next, err := cur.NewMessage("child")
		mustSet(t, err)
		cur = next
	}
	frame, err := typ.New().EncodeDelimited()
	mustSet(t, err)
	if len(frame) != 1 || frame[0] != 0 {
		t.Fatalf("empty frame = %x, want 00", frame)
	}
	if frame, err := root.EncodeDelimited(); err == nil || frame != nil {
		t.Fatalf("deep frame = %x, err %v; want nil, ErrDepth", frame, err)
	}
}

// A Timestamp carries only non-negative nanos; a peer that sends a negative
// count is rejected, like the C core's wkt.c.
func TestTimestampMillisRejectsNegativeNanos(t *testing.T) {
	s := loadWKTSchema(t)
	tsType := mustWKTType(t, s, "google.protobuf.Timestamp")
	m := tsType.New()
	mustSet(t, m.SetInt("seconds", -1))
	mustSet(t, m.SetInt("nanos", -1))
	if _, err := runtime.TimestampMillis(m); !errors.Is(err, runtime.ErrWire) {
		t.Fatalf("TimestampMillis accepted negative nanos: %v", err)
	}
	good := tsType.New()
	mustSet(t, good.SetInt("seconds", 1))
	mustSet(t, good.SetInt("nanos", 500000000))
	if ms, err := runtime.TimestampMillis(good); err != nil || ms != 1500 {
		t.Fatalf("TimestampMillis = %d (%v), want 1500", ms, err)
	}
}

// The text printer bounds unknown-field nesting, so hostile bytes fail with
// ErrDepth instead of exhausting the stack.
func TestTextUnknownNestingCap(t *testing.T) {
	raw, err := os.ReadFile("testdata/matrix3.desc")
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "structtest.Scalars3")

	// Unknown field 100, wire type 2, nested 300 deep: every payload is a
	// well-formed field run, so the printer recurses into each one.
	payload := []byte{0x08, 0x01}
	for i := 0; i < 300; i++ {
		inner := payload
		payload = append([]byte{0xa2, 0x06}, runtime.AppendVarint(nil, uint64(len(inner)))...)
		payload = append(payload, inner...)
	}
	m, err := typ.Decode(payload)
	mustSet(t, err)
	if _, err := typ.MarshalText(m, true); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300-deep unknown fields printed: %v", err)
	}
}

// The JSON printer bounds nesting too.
func TestJSONNestingCap(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("node.proto"),
		Package: jstrp("node"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("Node", nil,
			jfield("child", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".node.Node"))},
	})
	typ := mustType(t, s, "node.Node")
	root := typ.New()
	cur := root
	for i := 0; i < 300; i++ {
		next, err := cur.NewMessage("child")
		mustSet(t, err)
		cur = next
	}
	if _, err := typ.MarshalJSONString(root); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300-deep JSON tree printed: %v", err)
	}
}

// The descriptor's json_name option names the key on both sides.
func TestJSONNameOption(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("jn.proto"),
		Package: jstrp("jn"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("M", nil, &descriptorpb.FieldDescriptorProto{
			Name: jstrp("foo"), JsonName: jstrp("Zed"), Number: ji32p(1),
			Label: labp(jlabelOpt), Type: typp(descriptorpb.FieldDescriptorProto_TYPE_INT32),
		})},
	})
	typ := mustType(t, s, "jn.M")
	m := typ.New()
	mustSet(t, m.SetInt("foo", 7))
	out, err := typ.MarshalJSONString(m)
	mustSet(t, err)
	if out != `{"Zed":7}` {
		t.Fatalf("json_name ignored on print: %s", out)
	}
	for _, doc := range []string{`{"Zed":7}`, `{"foo":7}`} {
		back, err := typ.UnmarshalJSONString(doc, false)
		if err != nil {
			t.Fatalf("%s: %v", doc, err)
		}
		if v, _, _ := back.GetInt("foo"); v != 7 {
			t.Fatalf("%s read back %d", doc, v)
		}
	}
}

// A Struct decoded from wire may repeat a key; JSON objects cannot, so the
// printer keeps the last entry only.
func TestJSONStructDuplicateKeys(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	m := typ.New()
	st, err := m.NewMessage("st")
	mustSet(t, err)
	for _, v := range []string{"x", "y"} {
		e, err := st.NewMessage("fields")
		mustSet(t, err)
		mustSet(t, e.SetString("key", "a"))
		val, err := e.NewMessage("value")
		mustSet(t, err)
		mustSet(t, val.SetString("string_value", v))
	}
	out, err := typ.MarshalJSONString(m)
	mustSet(t, err)
	if want := `{"st":{"a":"y"}}`; out != want {
		t.Fatalf("duplicate Struct key printed %s, want %s", out, want)
	}
}

// Floats print with the reference's %.*g spelling, not Go's earlier
// exponent switch.
func TestJSONFloatSpelling(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	m := typ.New()
	mustSet(t, m.SetDouble("ratio", 123456789))
	o, err := typ.MarshalJSONString(m)
	mustSet(t, err)
	if !strings.Contains(o, `"ratio":123456789`) {
		t.Fatalf("double spelling: %s", o)
	}
	f := typ.New()
	mustSet(t, f.SetFloat("quant", 0.1))
	o, err = typ.MarshalJSONString(f)
	mustSet(t, err)
	if !strings.Contains(o, `"quant":0.100000001`) {
		t.Fatalf("float spelling: %s", o)
	}
}

// JSON carries extensions under "[full.name]" on both sides.
func TestJSONExtensionRoundTrip(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name:           jstrp("Holder"),
		Field:          []*descriptorpb.FieldDescriptorProto{jfield("base", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")},
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{{Start: ji32p(100), End: ji32p(200)}},
	}
	extra := jfield("extra", 101, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")
	extra.Extendee = jstrp(".jnx.Holder")
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("jnx.proto"),
		Package:     jstrp("jnx"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{holder},
		Extension:   []*descriptorpb.FieldDescriptorProto{extra},
	})
	typ := mustType(t, s, "jnx.Holder")
	m, err := typ.UnmarshalText("base: 1\n[jnx.extra]: 5\n")
	mustSet(t, err)
	out, err := typ.MarshalJSONString(m)
	mustSet(t, err)
	if out != `{"base":1,"[jnx.extra]":5}` {
		t.Fatalf("extension print: %s", out)
	}
	back, err := typ.UnmarshalJSONString(out, false)
	mustSet(t, err)
	wire, err := typ.Encode(back)
	mustSet(t, err)
	if want := []byte{0x08, 0x01, 0xa8, 0x06, 0x05}; !bytes.Equal(wire, want) {
		t.Fatalf("extension parse: %x, want %x", wire, want)
	}
	if _, err := typ.UnmarshalJSONString(`{"[jnx.nope]":1}`, false); err == nil {
		t.Fatal("unknown extension accepted")
	}
	if _, err := typ.UnmarshalJSONString(`{"base":1,"[jnx.extra]":5,"[jnx.extra]":6}`, false); err == nil {
		t.Fatal("duplicate extension accepted")
	}
}

// A proto2 string has no UTF-8 check on parse, matching the decoder's
// fp-driven rule.
func TestTextProto2StringUTF8(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest2.Req")
	m, err := typ.UnmarshalText("need: 1\nname: \"\\377\"\n")
	mustSet(t, err)
	if v, _, _ := m.GetString("name"); v != "\xff" {
		t.Fatalf("proto2 string = %q, want \\xff", v)
	}
}

// Any prints as its raw type_url/value fields; an unresolvable type must not
// swallow the data.
func TestTextAnyPrintsRawFields(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	m, err := typ.UnmarshalText(`any: { [type.googleapis.com/jsontest.M] { text: "hi" } }`)
	mustSet(t, err)
	out, err := typ.MarshalText(m, false)
	mustSet(t, err)
	if strings.Contains(out, "[type.googleapis.com") {
		t.Fatalf("Any expanded instead of printing raw fields:\n%s", out)
	}
	if !strings.Contains(out, "type_url:") || !strings.Contains(out, "value:") {
		t.Fatalf("Any lost its raw fields:\n%s", out)
	}
	sub, _, _ := m.GetMessage("any")
	mustSet(t, sub.SetString("type_url", "type.googleapis.com/jsontest.Nope"))
	out, err = typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("unresolvable Any did not print: %v", err)
	}
	if !strings.Contains(out, "jsontest.Nope") {
		t.Fatalf("unresolvable Any dropped its type:\n%s", out)
	}
	back, err := typ.UnmarshalText(out)
	mustSet(t, err)
	again, err := typ.MarshalText(back, false)
	mustSet(t, err)
	if again != out {
		t.Fatalf("Any print not stable:\n%s\nvs\n%s", out, again)
	}
}

// Any expansion rules mirror the reference: one expansion per Any, a URL
// with a non-empty prefix and a dotted type name.
func TestTextAnyExpansionRules(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	for _, doc := range []string{
		`any: { [jsontest.M] { } }`,
		`any: { [/jsontest.M] { } }`,
		`any: { [type.googleapis.com/jsontest..M] { } }`,
		`any: { [type.googleapis.com/jsontest.M] { } [other.M] { } }`,
	} {
		if _, err := typ.UnmarshalText(doc); err == nil {
			t.Fatalf("%s accepted", doc)
		}
	}
	if _, err := typ.UnmarshalText(`any: { [type.googleapis.com/jsontest.M] { text: "x" } }`); err != nil {
		t.Fatalf("valid expansion failed: %v", err)
	}
}

// A singular extension may only be named once.
func TestTextDuplicateExtension(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name:           jstrp("Holder"),
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{{Start: ji32p(100), End: ji32p(200)}},
	}
	extra := jfield("extra", 101, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")
	extra.Extendee = jstrp(".dup.Holder")
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("dup.proto"),
		Package:     jstrp("dup"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{holder},
		Extension:   []*descriptorpb.FieldDescriptorProto{extra},
	})
	typ := mustType(t, s, "dup.Holder")
	if _, err := typ.UnmarshalText("[dup.extra]: 5\n[dup.extra]: 6\n"); err == nil {
		t.Fatal("duplicate singular extension accepted")
	}
}

// A legal field number near MaxFieldNumber must not size the lookup index
// by its number alone (that would ask for gigabytes).
func TestSparseFieldNumberIndex(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("sparse.proto"),
		Package: jstrp("sparse"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("M", nil,
			jfield("small", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			jfield("huge", int32(runtime.MaxFieldNumber), jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))},
	})
	typ := mustType(t, s, "sparse.M")
	m := typ.New()
	mustSet(t, m.SetInt("small", 3))
	out, err := m.Encode()
	mustSet(t, err)
	if want := []byte{0x08, 0x03}; !bytes.Equal(out, want) {
		t.Fatalf("sparse table encode: %x, want %x", out, want)
	}
	hugeTag := []byte{0xf8, 0xff, 0xff, 0xff, 0x0f, 0x01}
	back, err := typ.Decode(append(out, hugeTag...))
	mustSet(t, err)
	if v, ok, err := back.GetInt("huge"); err != nil || !ok || v != 1 {
		t.Fatalf("sparse lookup huge = %d (%v)", v, err)
	}
}

// Binary decode lands extensions in their typed slots (like the C core),
// not just in unknown retention: decode -> JSON/text/encode all see them.
func TestBinaryDecodePopulatesExtensions(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name:           jstrp("H"),
		Field:          []*descriptorpb.FieldDescriptorProto{jfield("base", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")},
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{{Start: ji32p(100), End: ji32p(200)}},
		NestedType: []*descriptorpb.DescriptorProto{
			msgOf("Grp", nil, jfield("v", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
		},
	}
	box := msgOf("Box", nil, jfield("g", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))
	mkext := func(name string, num int32, ty descriptorpb.FieldDescriptorProto_Type, tn string) *descriptorpb.FieldDescriptorProto {
		e := jfield(name, num, jlabelOpt, ty, tn)
		e.Extendee = jstrp(".bx.H")
		return e
	}
	mkrep := func(name string, num int32, ty descriptorpb.FieldDescriptorProto_Type, tn string) *descriptorpb.FieldDescriptorProto {
		e := jfield(name, num, jlabelRep, ty, tn)
		e.Extendee = jstrp(".bx.H")
		return e
	}
	pkext := mkrep("pnums", 103, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")
	pkext.Options = &descriptorpb.FieldOptions{Packed: proto.Bool(true)}
	grp := mkext("grp", 104, descriptorpb.FieldDescriptorProto_TYPE_GROUP, ".bx.H.Grp")
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("bx.proto"),
		Package:     jstrp("bx"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{holder, box},
		Extension: []*descriptorpb.FieldDescriptorProto{
			mkext("s", 100, descriptorpb.FieldDescriptorProto_TYPE_STRING, ""),
			mkext("box", 101, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".bx.Box"),
			mkrep("nums", 102, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			pkext,
			grp,
		},
	})
	typ := mustType(t, s, "bx.H")
	m, err := typ.UnmarshalText("base: 1\n[bx.s]: \"hi\"\n[bx.box] { g: 7 }\n[bx.nums]: 1\n[bx.nums]: 2\n[bx.grp] { v: 3 }\n")
	mustSet(t, err)
	wire, err := typ.Encode(m)
	mustSet(t, err)
	back, err := typ.Decode(wire)
	mustSet(t, err)
	js, err := typ.MarshalJSONString(back)
	mustSet(t, err)
	if want := `{"base":1,"[bx.s]":"hi","[bx.box]":{"g":7},"[bx.nums]":[1,2],"[bx.grp]":{"v":3}}`; js != want {
		t.Fatalf("decode->JSON: %s, want %s", js, want)
	}
	tx, err := typ.MarshalText(back, false)
	mustSet(t, err)
	again, err := typ.UnmarshalText(tx)
	mustSet(t, err)
	wire2, err := typ.Encode(again)
	mustSet(t, err)
	if !bytes.Equal(wire, wire2) {
		t.Fatalf("decode->text->encode changed bytes: %x vs %x", wire, wire2)
	}
	// Packed extension bytes decode typed and stay packed.
	bp, err := typ.Decode([]byte{0xba, 0x06, 0x03, 0x01, 0x02, 0x03})
	mustSet(t, err)
	jp, err := typ.MarshalJSONString(bp)
	mustSet(t, err)
	if jp != `{"[bx.pnums]":[1,2,3]}` {
		t.Fatalf("packed ext JSON: %s", jp)
	}
	wp, err := typ.Encode(bp)
	mustSet(t, err)
	if !bytes.Equal(wp, []byte{0xba, 0x06, 0x03, 0x01, 0x02, 0x03}) {
		t.Fatalf("packed ext re-encode: %x", wp)
	}
}

// A delimited (editions message_encoding=DELIMITED) message field keeps its
// field name in text/JSON while traveling with group tags on the wire.
func TestDelimitedJSONText(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("delim2.proto"),
		Package: jstrp("d.Delim"),
		Syntax:  jstrp("editions"),
		MessageType: []*descriptorpb.DescriptorProto{
			msgOf("Sub", nil, jfield("v", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
			msgOf("M", nil, &descriptorpb.FieldDescriptorProto{
				Name: jstrp("data"), Number: ji32p(1), Label: labp(jlabelOpt),
				Type: typp(descriptorpb.FieldDescriptorProto_TYPE_MESSAGE), TypeName: jstrp(".d.Delim.Sub"),
				Options: &descriptorpb.FieldOptions{Features: &descriptorpb.FeatureSet{MessageEncoding: descriptorpb.FeatureSet_DELIMITED.Enum()}},
			}),
		},
	})
	typ := mustType(t, s, "d.Delim.M")
	m := typ.New()
	sub, err := m.NewMessage("data")
	mustSet(t, err)
	mustSet(t, sub.SetInt("v", 9))
	js, err := typ.MarshalJSONString(m)
	mustSet(t, err)
	if js != `{"data":{"v":9}}` {
		t.Fatalf("delimited JSON: %s", js)
	}
	back, err := typ.UnmarshalJSONString(js, false)
	mustSet(t, err)
	wire, err := typ.Encode(back)
	mustSet(t, err)
	if want := []byte{0x0b, 0x08, 0x09, 0x0c}; !bytes.Equal(wire, want) {
		t.Fatalf("delimited JSON->wire: %x, want %x", wire, want)
	}
	tx, err := typ.MarshalText(m, false)
	mustSet(t, err)
	back2, err := typ.UnmarshalText(tx)
	mustSet(t, err)
	wire2, err := typ.Encode(back2)
	mustSet(t, err)
	if !bytes.Equal(wire2, wire) {
		t.Fatalf("delimited text->wire: %x, want %x", wire2, wire)
	}
	if n := m.EncodedSize(); n != len(wire) {
		t.Fatalf("EncodedSize %d != len(encode) %d for a delimited field", n, len(wire))
	}
}

// Message-typed extensions ride JSON as objects, repeated ones as arrays,
// on both sides of the codec.
func TestMessageExtensionJSON(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name:           jstrp("H"),
		Field:          []*descriptorpb.FieldDescriptorProto{jfield("base", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")},
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{{Start: ji32p(100), End: ji32p(200)}},
	}
	box := msgOf("Box", nil, jfield("g", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))
	one := jfield("box", 100, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".mx.Box")
	one.Extendee = jstrp(".mx.H")
	rep := jfield("boxes", 101, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".mx.Box")
	rep.Extendee = jstrp(".mx.H")
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("mx.proto"),
		Package:     jstrp("mx"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{holder, box},
		Extension:   []*descriptorpb.FieldDescriptorProto{one, rep},
	})
	typ := mustType(t, s, "mx.H")
	m, err := typ.UnmarshalJSONString(`{"base":1,"[mx.box]":{"g":7},"[mx.boxes]":[{"g":1},{"g":2}]}`, false)
	mustSet(t, err)
	wire, err := typ.Encode(m)
	mustSet(t, err)
	back, err := typ.Decode(wire)
	mustSet(t, err)
	js, err := typ.MarshalJSONString(back)
	mustSet(t, err)
	if js != `{"base":1,"[mx.box]":{"g":7},"[mx.boxes]":[{"g":1},{"g":2}]}` {
		t.Fatalf("message ext JSON round trip: %s", js)
	}
}

// A huge unknown field number skips without sizing anything by its number,
// and a known huge field carrying the wrong wire type diverts to unknown
// bytes with its tag, like any other mistyped field.
func TestSparseUnknownFields(t *testing.T) {
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:    jstrp("sp.proto"),
		Package: jstrp("sp"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{msgOf("M", nil,
			jfield("small", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			jfield("huge", int32(runtime.MaxFieldNumber), jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))},
	})
	typ := mustType(t, s, "sp.M")
	m, err := typ.Decode([]byte{0x08, 0x01, 0x80, 0x80, 0x80, 0x80, 0x0f, 0x02})
	mustSet(t, err)
	out, err := m.Encode()
	mustSet(t, err)
	if want := []byte{0x08, 0x01, 0x80, 0x80, 0x80, 0x80, 0x0f, 0x02}; !bytes.Equal(out, want) {
		t.Fatalf("sparse unknown round trip: %x", out)
	}
	m2, err := typ.Decode([]byte{0x08, 0x01, 0xfa, 0xff, 0xff, 0xff, 0x0f, 0x01, 0x00})
	mustSet(t, err)
	out2, err := m2.Encode()
	mustSet(t, err)
	if want := []byte{0x08, 0x01, 0xfa, 0xff, 0xff, 0xff, 0x0f, 0x01, 0x00}; !bytes.Equal(out2, want) {
		t.Fatalf("diverted huge field: %x, want %x", out2, want)
	}
}

// A nil tree encodes as absent instead of panicking, like EncodedSize and
// IsInitialized already treat it.
func TestEncodeNilTree(t *testing.T) {
	var m *runtime.DMsg
	out, err := m.Encode()
	mustSet(t, err)
	if len(out) != 0 {
		t.Fatalf("nil tree encoded %x, want empty", out)
	}
	if _, err := m.EncodeDelimited(); err != nil {
		t.Fatalf("nil tree delimited: %v", err)
	}
}

// An editions message-typed extension with message_encoding=DELIMITED
// travels with group tags on the wire but keeps its extension name in
// text and JSON, exactly like a delimited regular field.
func TestDelimitedExtension(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name:           jstrp("H"),
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{{Start: ji32p(100), End: ji32p(200)}},
	}
	sub := msgOf("Sub", nil, jfield("v", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""))
	dx := &descriptorpb.FieldDescriptorProto{
		Name: jstrp("data"), Number: ji32p(100), Label: labp(jlabelOpt),
		Type: typp(descriptorpb.FieldDescriptorProto_TYPE_MESSAGE), TypeName: jstrp(".de.Sub"),
		Extendee: jstrp(".de.H"),
		Options:  &descriptorpb.FieldOptions{Features: &descriptorpb.FeatureSet{MessageEncoding: descriptorpb.FeatureSet_DELIMITED.Enum()}},
	}
	s := loadSet(t, &descriptorpb.FileDescriptorProto{
		Name:        jstrp("de.proto"),
		Package:     jstrp("de"),
		Syntax:      jstrp("editions"),
		MessageType: []*descriptorpb.DescriptorProto{holder, sub},
		Extension:   []*descriptorpb.FieldDescriptorProto{dx},
	})
	typ := mustType(t, s, "de.H")
	m, err := typ.UnmarshalText("[de.data] { v: 4 }\n")
	mustSet(t, err)
	wire, err := typ.Encode(m)
	mustSet(t, err)
	if want := []byte{0xa3, 0x06, 0x08, 0x04, 0xa4, 0x06}; !bytes.Equal(wire, want) {
		t.Fatalf("delimited ext encode: %x, want %x", wire, want)
	}
	if n := m.EncodedSize(); n != len(wire) {
		t.Fatalf("EncodedSize %d != len(encode) %d for a delimited extension", n, len(wire))
	}
	back, err := typ.Decode(wire)
	mustSet(t, err)
	js, err := typ.MarshalJSONString(back)
	mustSet(t, err)
	if js != `{"[de.data]":{"v":4}}` {
		t.Fatalf("delimited ext decode->JSON: %s", js)
	}
	wire2, err := typ.Encode(back)
	mustSet(t, err)
	if !bytes.Equal(wire, wire2) {
		t.Fatalf("delimited ext re-encode: %x vs %x", wire, wire2)
	}
}
