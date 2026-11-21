// Text format tests over the dynamic core. Table-driven over synthetic
// descriptors (built in-test) plus the matrix3/oneof1/maptest desc fixtures.
//
// Cases: full scalar round trip byte-for-byte, float specials, enum by name
// with open/closed unknown numbers, groups by short name, maps as entries,
// oneof and singular duplicate failures, unknown fail versus reserved skip,
// required-missing failures (proto2), and the lenient input spellings
// (hex/octal ints, C escapes, adjacent strings, <> delimiters,
// colon-optional messages, [a, b] lists, ','/';' separators, '#' comments).
package runtime_test

import (
	"bytes"
	"math"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"

	"pbwire/runtime"
)

// loadTextSchema builds a Schema holding texttest.M/Inner/Color (proto3
// with a group, a oneof and reserved names) plus texttest2.Req (proto2
// with a required field and a closed enum).
func loadTextSchema(t *testing.T) *runtime.Schema {
	t.Helper()
	opt := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	rep := descriptorpb.FieldDescriptorProto_LABEL_REPEATED
	req := descriptorpb.FieldDescriptorProto_LABEL_REQUIRED
	i32 := descriptorpb.FieldDescriptorProto_TYPE_INT32
	i64 := descriptorpb.FieldDescriptorProto_TYPE_INT64
	u32 := descriptorpb.FieldDescriptorProto_TYPE_UINT32
	u64 := descriptorpb.FieldDescriptorProto_TYPE_UINT64
	bl := descriptorpb.FieldDescriptorProto_TYPE_BOOL
	st := descriptorpb.FieldDescriptorProto_TYPE_STRING
	bt := descriptorpb.FieldDescriptorProto_TYPE_BYTES
	db := descriptorpb.FieldDescriptorProto_TYPE_DOUBLE
	fl := descriptorpb.FieldDescriptorProto_TYPE_FLOAT
	en := descriptorpb.FieldDescriptorProto_TYPE_ENUM
	ms := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE
	gp := descriptorpb.FieldDescriptorProto_TYPE_GROUP

	msg := func(name string, fields ...*descriptorpb.FieldDescriptorProto) *descriptorpb.DescriptorProto {
		return &descriptorpb.DescriptorProto{Name: jstrp(name), Field: fields}
	}
	inner := msg("Inner",
		jfield("name", 1, opt, st, ""),
		jfield("value", 2, opt, i32, ""))
	mygroup := msg("Mygroup",
		jfield("gv", 1, opt, i32, ""))
	m := msg("M",
		jfield("i32", 1, opt, i32, ""),
		jfield("i64", 2, opt, i64, ""),
		jfield("u32", 3, opt, u32, ""),
		jfield("u64", 4, opt, u64, ""),
		jfield("flag", 5, opt, bl, ""),
		jfield("str", 6, opt, st, ""),
		jfield("blob", 7, opt, bt, ""),
		jfield("ratio", 8, opt, db, ""),
		jfield("quant", 9, opt, fl, ""),
		jfield("color", 10, opt, en, ".texttest.Color"),
		jfield("sub", 11, opt, ms, ".texttest.Inner"),
		jfield("nums", 12, rep, i32, ""),
		jfield("tags", 13, rep, st, ""),
		jfield("subs", 14, rep, ms, ".texttest.Inner"),
		jfield("mygroup", 15, opt, gp, ".texttest.M.Mygroup"),
		jfield("num", 16, opt, i32, ""),
		jfield("text", 17, opt, st, ""),
		jfield("inner2", 18, opt, ms, ".texttest.Inner"))
	m.NestedType = []*descriptorpb.DescriptorProto{mygroup}
	m.OneofDecl = []*descriptorpb.OneofDescriptorProto{{Name: jstrp("choice")}}
	for _, n := range []string{"num", "text", "inner2"} {
		for _, f := range m.Field {
			if f.GetName() == n {
				f.OneofIndex = proto.Int32(0)
			}
		}
	}
	m.ReservedName = []string{"old_field", "legacy"}
	sub2 := msg("Sub2",
		jfield("req", 1, req, i32, ""))
	t2 := &descriptorpb.FileDescriptorProto{
		Name:    jstrp("t2.proto"),
		Package: jstrp("texttest2"),
		Syntax:  jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{
			msg("Req",
				jfield("need", 1, req, i32, ""),
				jfield("ec", 2, opt, en, ".texttest2.Closed"),
				jfield("name", 3, opt, st, ""),
				jfield("sub2", 4, opt, ms, ".texttest2.Sub2")),
			sub2,
		},
		EnumType: []*descriptorpb.EnumDescriptorProto{
			{
				Name: jstrp("Closed"),
				Value: []*descriptorpb.EnumValueDescriptorProto{
					{Name: jstrp("C_ZERO"), Number: ji32p(0)},
					{Name: jstrp("C_ONE"), Number: ji32p(1)},
				},
			},
		},
	}
	set := &descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{
		{
			Name:    jstrp("t.proto"),
			Package: jstrp("texttest"),
			Syntax:  jstrp("proto3"),
			MessageType: []*descriptorpb.DescriptorProto{
				msg("Target",
					jfield("name", 1, opt, st, ""),
					jfield("value", 2, opt, i32, "")),
				inner, m,
			},
			EnumType: []*descriptorpb.EnumDescriptorProto{
				{
					Name: jstrp("Color"),
					Value: []*descriptorpb.EnumValueDescriptorProto{
						{Name: jstrp("COLOR_UNSPECIFIED"), Number: ji32p(0)},
						{Name: jstrp("COLOR_RED"), Number: ji32p(1)},
						{Name: jstrp("COLOR_GREEN"), Number: ji32p(2)},
					},
				},
			},
		},
		t2,
	}}
	raw, err := proto.Marshal(set)
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// fillTextM builds the full-coverage message the round-trip case prints.
func fillTextM(t *testing.T, typ *runtime.DType) *runtime.DMsg {
	t.Helper()
	m := typ.New()
	mustSet(t, m.SetInt("i32", -7))
	mustSet(t, m.SetInt("i64", math.MinInt64))
	mustSet(t, m.SetUint("u32", math.MaxUint32))
	mustSet(t, m.SetUint("u64", math.MaxUint64))
	mustSet(t, m.SetBool("flag", true))
	mustSet(t, m.SetString("str", "hi \"yo\" \\ back\n\t\x01caf\u00e9"))
	mustSet(t, m.SetBytes("blob", []byte{0x00, 0x01, 0x02, 0xfa, 0xff}))
	mustSet(t, m.SetDouble("ratio", 1.5))
	mustSet(t, m.SetFloat("quant", 2.5))
	mustSet(t, m.SetInt("color", 2))
	sub, err := m.NewMessage("sub")
	mustSet(t, err)
	mustSet(t, sub.SetString("name", "kid"))
	mustSet(t, sub.SetInt("value", 42))
	mustSet(t, m.SetInt("nums", 1))
	mustSet(t, m.SetInt("nums", -2))
	mustSet(t, m.SetString("tags", "a"))
	mustSet(t, m.SetString("tags", "b"))
	s1, err := m.NewMessage("subs")
	mustSet(t, err)
	mustSet(t, s1.SetString("name", "s1"))
	g, err := m.NewMessage("mygroup")
	mustSet(t, err)
	mustSet(t, g.SetInt("gv", 9))
	mustSet(t, m.SetInt("num", 5))
	return m
}

const wantTextM = "i32: -7\n" +
	"i64: -9223372036854775808\n" +
	"u32: 4294967295\n" +
	"u64: 18446744073709551615\n" +
	"flag: true\n" +
	"str: \"hi \\\"yo\\\" \\\\ back\\n\\t\\001caf\\303\\251\"\n" +
	"blob: \"\\000\\001\\002\\372\\377\"\n" +
	"ratio: 1.5\n" +
	"quant: 2.5\n" +
	"color: COLOR_GREEN\n" +
	"sub {\n" +
	"  name: \"kid\"\n" +
	"  value: 42\n" +
	"}\n" +
	"nums: 1\n" +
	"nums: -2\n" +
	"tags: \"a\"\n" +
	"tags: \"b\"\n" +
	"subs {\n" +
	"  name: \"s1\"\n" +
	"}\n" +
	"Mygroup {\n" +
	"  gv: 9\n" +
	"}\n" +
	"num: 5\n"

func TestTextScalarsRoundTrip(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	m := fillTextM(t, typ)
	got, err := typ.MarshalText(m, true)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	if got != wantTextM {
		t.Fatalf("print:\n got:\n%s\nwant:\n%s", got, wantTextM)
	}
	back, err := typ.UnmarshalText(got)
	if err != nil {
		t.Fatalf("UnmarshalText: %v", err)
	}
	again, err := typ.MarshalText(back, true)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != wantTextM {
		t.Fatalf("round trip:\n got:\n%s\nwant:\n%s", again, wantTextM)
	}
	if v, ok, err := back.GetInt("i64"); err != nil || !ok || v != math.MinInt64 {
		t.Fatalf("i64 = %d ok=%v err=%v", v, ok, err)
	}
}

func TestTextLeniency(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	// Hex/octal/negative-hex ints, int-as-bool, float exponents and suffixes,
	// enum numbers, angle delimiters, lists, separators, comments, adjacent
	// strings and colon-optional messages.
	text := "# leading comment\n" +
		"i32: 0x10\n" +
		"u32: 010\n" +
		"i64: -0x1F\n" +
		"flag: 1\n" +
		"ratio: 1e3\n" +
		"quant: 2.5f\n" +
		"color: 1\n" +
		"sub <name: \"kid\"; value: 42,>\n" +
		"nums: [1, 2]\n" +
		"tags: \"a\"\n" +
		"tags: 'b'\n" +
		"str: \"x\" \"y\"\n"
	m, err := typ.UnmarshalText(text)
	if err != nil {
		t.Fatalf("UnmarshalText: %v", err)
	}
	if v, ok, _ := m.GetInt("i32"); !ok || v != 16 {
		t.Fatalf("i32 = %d ok=%v", v, ok)
	}
	if v, ok, _ := m.GetUint("u32"); !ok || v != 8 {
		t.Fatalf("u32 = %d ok=%v", v, ok)
	}
	if v, ok, _ := m.GetInt("i64"); !ok || v != -31 {
		t.Fatalf("i64 = %d ok=%v", v, ok)
	}
	if v, ok, _ := m.GetBool("flag"); !ok || !v {
		t.Fatalf("flag = %v ok=%v", v, ok)
	}
	if v, ok, _ := m.GetDouble("ratio"); !ok || v != 1000 {
		t.Fatalf("ratio = %v ok=%v", v, ok)
	}
	if v, ok, _ := m.GetFloat("quant"); !ok || v != 2.5 {
		t.Fatalf("quant = %v ok=%v", v, ok)
	}
	if v, ok, _ := m.GetInt("color"); !ok || v != 1 {
		t.Fatalf("color = %d ok=%v", v, ok)
	}
	sub, ok, err := m.GetMessage("sub")
	if err != nil || !ok {
		t.Fatalf("sub ok=%v err=%v", ok, err)
	}
	if v, ok, _ := sub.GetString("name"); !ok || v != "kid" {
		t.Fatalf("sub.name = %q ok=%v", v, ok)
	}
	got, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	want := "i32: 16\n" +
		"i64: -31\n" +
		"u32: 8\n" +
		"flag: true\n" +
		"str: \"xy\"\n" +
		"ratio: 1000\n" +
		"quant: 2.5\n" +
		"color: COLOR_RED\n" +
		"sub {\n" +
		"  name: \"kid\"\n" +
		"  value: 42\n" +
		"}\n" +
		"nums: 1\n" +
		"nums: 2\n" +
		"tags: \"a\"\n" +
		"tags: \"b\"\n"
	if got != want {
		t.Fatalf("reprint:\n got:\n%s\nwant:\n%s", got, want)
	}
	// Float specials spell and round-trip.
	for _, tc := range [][2]string{
		{"ratio: inf\n", "ratio: inf\n"},
		{"ratio: -inf\n", "ratio: -inf\n"},
		{"ratio: nan\n", "ratio: nan\n"},
		{"flag: True\n", "flag: true\n"},
		{"flag: f\n", ""},
		{"color: COLOR_GREEN\n", "color: COLOR_GREEN\n"},
	} {
		mm, err := typ.UnmarshalText(tc[0])
		if err != nil {
			t.Fatalf("%q: %v", tc[0], err)
		}
		out, err := typ.MarshalText(mm, false)
		if err != nil {
			t.Fatalf("%q reprint: %v", tc[0], err)
		}
		if out != tc[1] {
			t.Fatalf("%q reprint = %q, want %q", tc[0], out, tc[1])
		}
	}
	// The group short name and the proto field name both parse.
	for _, text := range []string{"Mygroup { gv: 9 }\n", "mygroup { gv: 9 }\n", "mygroup: { gv: 9 }\n"} {
		mm, err := typ.UnmarshalText(text)
		if err != nil {
			t.Fatalf("%q: %v", text, err)
		}
		out, err := typ.MarshalText(mm, false)
		if err != nil {
			t.Fatalf("%q reprint: %v", text, err)
		}
		if out != "Mygroup {\n  gv: 9\n}\n" {
			t.Fatalf("%q reprint = %q", text, out)
		}
	}
}

func TestTextEnumNumbers(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	// Unknown numbers on an open (proto3) enum print back as numbers.
	m, err := typ.UnmarshalText("color: 99\n")
	if err != nil {
		t.Fatalf("open unknown number: %v", err)
	}
	if v, ok, err := m.GetInt("color"); err != nil || !ok || v != 99 {
		t.Fatalf("color = %d ok=%v err=%v", v, ok, err)
	}
	out, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != "color: 99\n" {
		t.Fatalf("open unknown reprint = %q", out)
	}
	// Unknown numbers on a closed (proto2) enum fail.
	req := mustType(t, s, "texttest2.Req")
	if _, err := req.UnmarshalText("need: 1\nec: 99\n"); err == nil {
		t.Fatalf("closed unknown enum number must fail")
	}
	// Unknown names fail.
	if _, err := typ.UnmarshalText("color: NOPE\n"); err == nil {
		t.Fatalf("unknown enum name must fail")
	}
}

func TestTextAnyRoundTrip(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	m, err := typ.UnmarshalText(`any: { [type.googleapis.com/jsontest.M] { text: "hi" } }`)
	if err != nil {
		t.Fatalf("UnmarshalText Any: %v", err)
	}
	sub, ok, err := m.GetMessage("any")
	if err != nil || !ok || sub == nil {
		t.Fatalf("any: ok=%v err=%v", ok, err)
	}
	got, _, err := sub.GetString("type_url")
	if err != nil || got != "type.googleapis.com/jsontest.M" {
		t.Fatalf("type_url = %q (%v)", got, err)
	}
	inner, err := runtime.AnyUnpack(sub, mustType(t, s, "jsontest.M"))
	if err != nil {
		t.Fatalf("AnyUnpack: %v", err)
	}
	if v, _, err := inner.GetString("text"); err != nil || v != "hi" {
		t.Fatalf("text = %q (%v), want hi", v, err)
	}
	out, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	back, err := typ.UnmarshalText(out)
	if err != nil {
		t.Fatalf("reparse %q: %v", out, err)
	}
	again, err := typ.MarshalText(back, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != again {
		t.Fatalf("print not stable:\n%s\nvs\n%s", out, again)
	}
	// A custom URL prefix resolves by suffix and packs verbatim.
	m2, err := typ.UnmarshalText(`any: { [non.default.domain/jsontest.M] { text: "x" } }`)
	if err != nil {
		t.Fatalf("custom prefix: %v", err)
	}
	sub2, _, _ := m2.GetMessage("any")
	if u, _, _ := sub2.GetString("type_url"); u != "non.default.domain/jsontest.M" {
		t.Fatalf("type_url = %q", u)
	}
	// An unresolvable packed type fails the parse, like the reference.
	if _, err := typ.UnmarshalText(`any: { [type.googleapis.com/jsontest.Nope] { } }`); err == nil {
		t.Fatalf("unknown Any type must fail")
	}
	// An invalid percent-escape in the URL fails too, while a valid one
	// packs verbatim.
	if _, err := typ.UnmarshalText(`any: { [non.default.domain/%ZZ/jsontest.M] { } }`); err == nil {
		t.Fatalf("invalid percent-escape must fail")
	}
	m3, err := typ.UnmarshalText(`any: { [non.default.domain/%2F/jsontest.M] { text: "e" } }`)
	if err != nil {
		t.Fatalf("valid percent-escape: %v", err)
	}
	sub3, _, _ := m3.GetMessage("any")
	if u, _, _ := sub3.GetString("type_url"); u != "non.default.domain/%2F/jsontest.M" {
		t.Fatalf("type_url = %q", u)
	}
}

func TestTextFloatEdge(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	// ratio is field 11 (double), quant is field 12 (float).
	negZero := func(text string, want uint64) {
		t.Helper()
		m, err := typ.UnmarshalText(text)
		if err != nil {
			t.Fatalf("%s: %v", text, err)
		}
		wire, err := typ.Encode(m)
		if err != nil {
			t.Fatalf("%s encode: %v", text, err)
		}
		if len(wire) != 9 {
			t.Fatalf("%s encodes %d bytes, want 9 (tag + 8)", text, len(wire))
		}
		var got uint64
		for i := 0; i < 8; i++ {
			got |= uint64(wire[1+i]) << uint(8*i)
		}
		if got != want {
			t.Fatalf("%s bits %016x, want %016x", text, got, want)
		}
		if n := m.EncodedSize(); n != len(wire) {
			t.Fatalf("%s size %d != len %d", text, n, len(wire))
		}
	}
	negZero(`ratio: -0`, 1<<63)
	negZero(`ratio: -0.0`, 1<<63)
	negZero(`ratio: -1e-9999`, 1<<63)
	infs := []struct {
		text string
		bits uint64
	}{
		{`ratio: 1e9999`, 0x7FF0000000000000},
		{`ratio: -1e9999`, 0xFFF0000000000000},
		{`ratio: 1e18446744073709551616`, 0x7FF0000000000000},
		{`quant: 1e18446744073709551616`, 0x7FF0000000000000},
	}
	for _, tc := range infs {
		m, err := typ.UnmarshalText(tc.text)
		if err != nil {
			t.Fatalf("%s: %v", tc.text, err)
		}
		var bits uint64
		if strings.HasPrefix(tc.text, "quant") {
			v, _, err := m.GetFloat("quant")
			if err != nil {
				t.Fatal(err)
			}
			bits = uint64(math.Float64bits(float64(v)))
		} else {
			v, _, err := m.GetDouble("ratio")
			if err != nil {
				t.Fatal(err)
			}
			bits = math.Float64bits(v)
		}
		if bits != tc.bits {
			t.Fatalf("%s bits %016x, want %016x", tc.text, bits, tc.bits)
		}
	}
}

func TestTextExtensionRoundTrip(t *testing.T) {
	holder := &descriptorpb.DescriptorProto{
		Name: jstrp("Holder"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("base", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
		},
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{
			{Start: ji32p(100), End: ji32p(200)},
		},
	}
	groupBox := &descriptorpb.DescriptorProto{
		Name: jstrp("GroupBox"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("g", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
		},
	}
	groupExt := jfield("groupbox", 100, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_GROUP, ".exttest.GroupBox")
	groupExt.Extendee = jstrp(".exttest.Holder")
	intExt := jfield("extra", 101, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")
	intExt.Extendee = jstrp(".exttest.Holder")
	fd := &descriptorpb.FileDescriptorProto{
		Name:        jstrp("ext.proto"),
		Package:     jstrp("exttest"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{holder, groupBox},
		Extension:   []*descriptorpb.FieldDescriptorProto{groupExt, intExt},
	}
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{fd}})
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "exttest.Holder")
	m, err := typ.UnmarshalText(`base: 1 [exttest.groupbox] { g: 7 } [exttest.extra]: 5`)
	if err != nil {
		t.Fatalf("UnmarshalText extension: %v", err)
	}
	wire, err := typ.Encode(m)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	// Group extension frames with start/end tags, scalars ride varints.
	want := []byte{0x08, 0x01, 0xA3, 0x06, 0x08, 0x07, 0xA4, 0x06, 0xA8, 0x06, 0x05}
	if !bytes.Equal(wire, want) {
		t.Fatalf("encode % x, want % x", wire, want)
	}
	if n := m.EncodedSize(); n != len(wire) {
		t.Fatalf("EncodedSize %d != len(encode) %d", n, len(wire))
	}
	out, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	back, err := typ.UnmarshalText(out)
	if err != nil {
		t.Fatalf("reparse %q: %v", out, err)
	}
	again, err := typ.MarshalText(back, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != again {
		t.Fatalf("print not stable:\n%s\nvs\n%s", out, again)
	}
	if _, err := typ.UnmarshalText(`[exttest.nope]: 1`); err == nil {
		t.Fatalf("unknown extension must fail")
	}
	if _, err := typ.UnmarshalText(`[exttest.extra]: 1 [other.Holder]: 2`); err == nil {
		t.Fatalf("extension of another message must fail")
	}
}

// TestNestedExtensionEncode is the regression test for nested extensions
// dropped on binary encode: encodeExtTable used to run only at the top
// level, so a text-parsed extension inside a submessage vanished on
// Encode while EncodedSize still counted it. Every nested encode arm now
// shares encodeSubmessage, so the bytes and the size agree again.
func TestNestedExtensionEncode(t *testing.T) {
	inner := &descriptorpb.DescriptorProto{
		Name: jstrp("Inner"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("base", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
		},
		ExtensionRange: []*descriptorpb.DescriptorProto_ExtensionRange{
			{Start: ji32p(100), End: ji32p(200)},
		},
	}
	outer := &descriptorpb.DescriptorProto{
		Name: jstrp("Outer"),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("sub", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".nestext.Inner"),
		},
	}
	extra := jfield("extra", 101, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")
	extra.Extendee = jstrp(".nestext.Inner")
	fd := &descriptorpb.FileDescriptorProto{
		Name:        jstrp("nestext.proto"),
		Package:     jstrp("nestext"),
		Syntax:      jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{outer, inner},
		Extension:   []*descriptorpb.FieldDescriptorProto{extra},
	}
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{fd}})
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "nestext.Outer")
	m, err := typ.UnmarshalText(`sub { base: 1 [nestext.extra]: 5 }`)
	if err != nil {
		t.Fatalf("UnmarshalText nested extension: %v", err)
	}
	wire, err := typ.Encode(m)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	// Outer field 1 holds 5 inner bytes: base varint plus ext 101 varint 5.
	want := []byte{0x0A, 0x05, 0x08, 0x01, 0xA8, 0x06, 0x05}
	if !bytes.Equal(wire, want) {
		t.Fatalf("nested encode % x, want % x", wire, want)
	}
	if n := m.EncodedSize(); n != len(wire) {
		t.Fatalf("EncodedSize %d != len(encode) %d", n, len(wire))
	}
	dwire, err := typ.EncodeDeterministic(m)
	if err != nil {
		t.Fatalf("EncodeDeterministic: %v", err)
	}
	if !bytes.Equal(dwire, want) {
		t.Fatalf("nested deterministic encode % x, want % x", dwire, want)
	}
}

func TestTextUnknownAndReserved(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	// Unknown field names fail, including numeric lines and brackets.
	for _, text := range []string{"bogus: 1\n", "5: 123\n", "[full.name]: 1\n", "sub { bogus: 1 }\n"} {
		if _, err := typ.UnmarshalText(text); err == nil {
			t.Fatalf("%q must fail", text)
		}
	}
	// Reserved names are skipped silently, whatever the value shape.
	m, err := typ.UnmarshalText("old_field: 1\nlegacy { a: 1 }\nlegacy: [1, 2]\nlegacy: \"s\"\nflag: true\n")
	if err != nil {
		t.Fatalf("reserved skipped: %v", err)
	}
	if v, ok, err := m.GetBool("flag"); err != nil || !ok || !v {
		t.Fatalf("flag = %v ok=%v err=%v", v, ok, err)
	}
	// Unknown wire bytes print as numeric lines only when asked.
	wire := runtime.AppendVarint(runtime.AppendTag(nil, 99, 0), 7)
	wire = append(wire, runtime.AppendTag(nil, 100, 2)...)
	wire = append(wire, runtime.AppendLen(nil, []byte("xyz"))...)
	fx := runtime.AppendTag(nil, 103, 1)
	fx = append(fx, 8, 7, 6, 5, 4, 3, 2, 1)
	wire = append(wire, fx...)
	matrix := mustType(t, s, "texttest.M")
	dm, err := matrix.Decode(wire)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	plain, err := typ.MarshalText(dm, false)
	if err != nil {
		t.Fatalf("print without unknowns: %v", err)
	}
	if plain != "" {
		t.Fatalf("print without unknowns = %q", plain)
	}
	with, err := typ.MarshalText(dm, true)
	if err != nil {
		t.Fatalf("print with unknowns: %v", err)
	}
	if with != "99: 7\n100: \"xyz\"\n103: 0x0102030405060708\n" {
		t.Fatalf("unknown print = %q", with)
	}
	// Unknown numeric lines do not round-trip.
	if _, err := typ.UnmarshalText(with); err == nil {
		t.Fatalf("unknown numeric lines must not parse")
	}
}

func TestTextSingularTwice(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	// Singular scalars, singular messages and oneof members name once.
	for _, text := range []string{
		"i32: 1\ni32: 2\n",
		"sub { name: \"a\" }\nsub { name: \"b\" }\n",
		"num: 1\nnum: 2\n",
		"num: 1\ntext: \"x\"\n",
		"nums: [1,]\n",
		"i32 5\n",
	} {
		if _, err := typ.UnmarshalText(text); err == nil {
			t.Fatalf("%q must fail", text)
		}
	}
	// Repeated fields append across lines and lists.
	m, err := typ.UnmarshalText("nums: 1\nnums: [2, 3]\nsubs { name: \"a\" }\nsubs [{ name: \"b\" }]\n")
	if err != nil {
		t.Fatalf("repeated append: %v", err)
	}
	out, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	want := "nums: 1\nnums: 2\nnums: 3\nsubs {\n  name: \"a\"\n}\nsubs {\n  name: \"b\"\n}\n"
	if out != want {
		t.Fatalf("repeated reprint:\n got:\n%s\nwant:\n%s", out, want)
	}
}

func TestTextRequired(t *testing.T) {
	s := loadTextSchema(t)
	req := mustType(t, s, "texttest2.Req")
	if _, err := req.UnmarshalText("name: \"x\"\n"); err == nil {
		t.Fatalf("missing required field must fail")
	}
	m, err := req.UnmarshalText("need: 5\n")
	if err != nil {
		t.Fatalf("required present: %v", err)
	}
	if v, ok, err := m.GetInt("need"); err != nil || !ok || v != 5 {
		t.Fatalf("need = %d ok=%v err=%v", v, ok, err)
	}
	// Required inside a submessage is checked too.
	if _, err := req.UnmarshalText("need: 1\nsub2 { }\n"); err == nil {
		t.Fatalf("missing nested required field must fail")
	}
	okm, err := req.UnmarshalText("need: 1\nsub2 { req: 2 }\n")
	if err != nil {
		t.Fatalf("nested required present: %v", err)
	}
	out, err := req.MarshalText(okm, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != "need: 1\nsub2 {\n  req: 2\n}\n" {
		t.Fatalf("required reprint = %q", out)
	}
}

func TestTextOneofFixture(t *testing.T) {
	raw := mustReadFile(t, "testdata/oneof1.desc")
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ := mustType(t, s, "spectest.Oneof1")
	m := typ.New()
	mustSet(t, m.SetString("name", "n"))
	mustSet(t, m.SetInt("num", 5))
	got, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	if got != "name: \"n\"\nnum: 5\n" {
		t.Fatalf("oneof print = %q", got)
	}
	back, err := typ.UnmarshalText(got)
	if err != nil {
		t.Fatalf("UnmarshalText: %v", err)
	}
	again, err := typ.MarshalText(back, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != got {
		t.Fatalf("round trip = %q, want %q", again, got)
	}
	// Two members of one oneof fail, even across message values.
	for _, text := range []string{"num: 5\ntext: \"t\"\n", "inner { v: 1 }\nnum: 2\n"} {
		if _, err := typ.UnmarshalText(text); err == nil {
			t.Fatalf("%q must fail", text)
		}
	}
}

func TestTextMatrix3Fixture(t *testing.T) {
	raw := mustReadFile(t, "testdata/matrix3.desc")
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ := mustType(t, s, "structtest.Scalars3")
	m := typ.New()
	mustSet(t, m.SetInt("i32", -7))
	mustSet(t, m.SetString("str", "hello"))
	mustSet(t, m.SetInt("color", 2))
	got, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	if got != "i32: -7\nstr: \"hello\"\ncolor: COLOR3_GREEN\n" {
		t.Fatalf("matrix3 print = %q", got)
	}
	back, err := typ.UnmarshalText(got)
	if err != nil {
		t.Fatalf("UnmarshalText: %v", err)
	}
	if v, ok, _ := back.GetInt("i32"); !ok || v != -7 {
		t.Fatalf("i32 = %d ok=%v", v, ok)
	}
	// The empty message prints empty and parses back to empty.
	empty, err := typ.UnmarshalText("")
	if err != nil {
		t.Fatalf("empty: %v", err)
	}
	out, err := typ.MarshalText(empty, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != "" {
		t.Fatalf("empty print = %q", out)
	}
}

func TestTextMapFixture(t *testing.T) {
	raw := mustReadFile(t, "testdata/maptest.desc")
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ := mustType(t, s, "spectest.MapTest")
	m := typ.New()
	e1, err := m.NewMessage("by_int")
	mustSet(t, err)
	mustSet(t, e1.SetInt("key", 1))
	mustSet(t, e1.SetString("value", "one"))
	e2, err := m.NewMessage("by_str")
	mustSet(t, err)
	mustSet(t, e2.SetString("key", "k"))
	mustSet(t, e2.SetInt("value", 2))
	got, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	want := "by_int {\n  key: 1\n  value: \"one\"\n}\nby_str {\n  key: \"k\"\n  value: 2\n}\n"
	if got != want {
		t.Fatalf("map print:\n got:\n%s\nwant:\n%s", got, want)
	}
	back, err := typ.UnmarshalText(got)
	if err != nil {
		t.Fatalf("UnmarshalText: %v", err)
	}
	again, err := typ.MarshalText(back, false)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != want {
		t.Fatalf("map round trip:\n got:\n%s\nwant:\n%s", again, want)
	}
}

func TestTextGroupName(t *testing.T) {
	s := loadTextSchema(t)
	typ := mustType(t, s, "texttest.M")
	// Groups print under the unqualified type name, one element per line.
	m := typ.New()
	g, err := m.NewMessage("mygroup")
	mustSet(t, err)
	mustSet(t, g.SetInt("gv", -3))
	got, err := typ.MarshalText(m, false)
	if err != nil {
		t.Fatalf("MarshalText: %v", err)
	}
	if got != "Mygroup {\n  gv: -3\n}\n" {
		t.Fatalf("group print = %q", got)
	}
	if strings.Contains(got, "mygroup") {
		t.Fatalf("group must not print the proto field name: %q", got)
	}
}
