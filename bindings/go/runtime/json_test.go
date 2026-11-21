// JSON mapping tests over the dynamic core. Table-driven over synthetic
// descriptors (built in-test, with real well-known-type files pulled from
// the protobuf module), plus the matrix3/oneof1/maptest desc fixtures.
//
// Cases: full scalar round trip byte-for-byte, quoted i64, base64 bytes,
// enum-by-name with unknown open/closed numbers, maps as objects, Any
// pack-JSON-expand, Timestamp/Duration forms, unknown fail/ignore,
// required-missing fails (proto2), oneof/map duplicate rules, and the
// legacy parser tolerances (single quotes, trailing commas, bare keys,
// null, quoted scalars, nested-array flattening).
package runtime_test

import (
	"math"
	"os"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/known/anypb"
	"google.golang.org/protobuf/types/known/durationpb"
	"google.golang.org/protobuf/types/known/emptypb"
	"google.golang.org/protobuf/types/known/fieldmaskpb"
	"google.golang.org/protobuf/types/known/structpb"
	"google.golang.org/protobuf/types/known/timestamppb"
	"google.golang.org/protobuf/types/known/wrapperspb"

	"pbwire/runtime"
)

func jstrp(s string) *string { return &s }

func ji32p(i int32) *int32 { return &i }

func labp(l descriptorpb.FieldDescriptorProto_Label) *descriptorpb.FieldDescriptorProto_Label {
	return &l
}

func typp(t descriptorpb.FieldDescriptorProto_Type) *descriptorpb.FieldDescriptorProto_Type {
	return &t
}

func jfield(name string, num int32, l descriptorpb.FieldDescriptorProto_Label, t descriptorpb.FieldDescriptorProto_Type, typeName string) *descriptorpb.FieldDescriptorProto {
	f := &descriptorpb.FieldDescriptorProto{
		Name:   jstrp(name),
		Number: ji32p(num),
		Label:  labp(l),
		Type:   typp(t),
	}
	if typeName != "" {
		f.TypeName = jstrp(typeName)
	}
	return f
}

const (
	jlabelOpt = descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	jlabelReq = descriptorpb.FieldDescriptorProto_LABEL_REQUIRED
	jlabelRep = descriptorpb.FieldDescriptorProto_LABEL_REPEATED
)

func jmapEntry(name, keyType, valType string, valMsg string) *descriptorpb.DescriptorProto {
	keyT := descriptorpb.FieldDescriptorProto_TYPE_STRING
	if keyType == "int64" {
		keyT = descriptorpb.FieldDescriptorProto_TYPE_INT64
	}
	valT := descriptorpb.FieldDescriptorProto_TYPE_INT32
	if valType == "string" {
		valT = descriptorpb.FieldDescriptorProto_TYPE_STRING
	}
	entry := &descriptorpb.DescriptorProto{
		Name: jstrp(name),
		Field: []*descriptorpb.FieldDescriptorProto{
			jfield("key", 1, jlabelOpt, keyT, ""),
			jfield("value", 2, jlabelOpt, valT, valMsg),
		},
		Options: &descriptorpb.MessageOptions{MapEntry: proto.Bool(true)},
	}
	return entry
}

// loadJSONSchema builds a Schema holding jsontest.M, jsontest.W and
// jsontest2.Req plus every well-known type the JSON mapping special-cases.
func loadJSONSchema(t *testing.T) *runtime.Schema {
	t.Helper()
	wkts := []protoreflect.FileDescriptor{
		anypb.File_google_protobuf_any_proto,
		timestamppb.File_google_protobuf_timestamp_proto,
		durationpb.File_google_protobuf_duration_proto,
		structpb.File_google_protobuf_struct_proto,
		fieldmaskpb.File_google_protobuf_field_mask_proto,
		wrapperspb.File_google_protobuf_wrappers_proto,
		emptypb.File_google_protobuf_empty_proto,
	}
	set := &descriptorpb.FileDescriptorSet{}
	for _, fd := range wkts {
		set.File = append(set.File, protodesc.ToFileDescriptorProto(fd))
	}
	msg := func(name string, fields ...*descriptorpb.FieldDescriptorProto) *descriptorpb.DescriptorProto {
		return &descriptorpb.DescriptorProto{Name: jstrp(name), Field: fields}
	}
	jtest := &descriptorpb.FileDescriptorProto{
		Name:       jstrp("jtest.proto"),
		Package:    jstrp("jsontest"),
		Syntax:     jstrp("proto3"),
		Dependency: []string{"google/protobuf/any.proto", "google/protobuf/timestamp.proto", "google/protobuf/duration.proto", "google/protobuf/struct.proto", "google/protobuf/field_mask.proto", "google/protobuf/wrappers.proto", "google/protobuf/empty.proto"},
		MessageType: []*descriptorpb.DescriptorProto{
			msg("Target",
				jfield("name", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_STRING, ""),
				jfield("value", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
			msg("Inner",
				jfield("name", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_STRING, ""),
				jfield("value", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, "")),
			{
				Name: jstrp("M"),
				Field: []*descriptorpb.FieldDescriptorProto{
					jfield("big_id", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT64, ""),
					jfield("huge", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_UINT64, ""),
					jfield("text", 3, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_STRING, ""),
					jfield("blob", 4, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_BYTES, ""),
					jfield("color", 5, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_ENUM, ".jsontest.Color"),
					jfield("sub", 6, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".jsontest.Inner"),
					jfield("nums", 7, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
					jfield("scores", 8, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".jsontest.M.ScoresEntry"),
					jfield("by_id", 9, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".jsontest.M.ByIdEntry"),
					jfield("flag", 10, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_BOOL, ""),
					jfield("ratio", 11, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_DOUBLE, ""),
					jfield("quant", 12, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_FLOAT, ""),
					jfield("target", 13, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".jsontest.Target"),
				},
				NestedType: []*descriptorpb.DescriptorProto{
					jmapEntry("ScoresEntry", "string", "int32", ""),
					jmapEntry("ByIdEntry", "int64", "string", ""),
				},
			},
			msg("W",
				jfield("ts", 1, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Timestamp"),
				jfield("dur", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Duration"),
				jfield("any", 3, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Any"),
				jfield("st", 4, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Struct"),
				jfield("fm", 5, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.FieldMask"),
				jfield("wi", 6, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Int32Value"),
				jfield("e", 7, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Empty"),
				jfield("lv", 8, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.ListValue"),
				jfield("v", 9, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.Value"),
				jfield("ws", 10, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".google.protobuf.StringValue")),
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
	}
	jtest2 := &descriptorpb.FileDescriptorProto{
		Name:    jstrp("jtest2.proto"),
		Package: jstrp("jsontest2"),
		Syntax:  jstrp("proto2"),
		MessageType: []*descriptorpb.DescriptorProto{
			msg("Req",
				jfield("need", 1, jlabelReq, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
				jfield("ec", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_ENUM, ".jsontest2.Closed"),
				jfield("name", 3, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_STRING, "")),
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
	set.File = append(set.File, jtest, jtest2)
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

func mustReadFile(t *testing.T, path string) []byte {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func mustType(t *testing.T, s *runtime.Schema, full string) *runtime.DType {
	t.Helper()
	typ, err := s.Type(full)
	if err != nil {
		t.Fatal(err)
	}
	return typ
}

func mustSet(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

// fillJSONM builds the full Scalars-style message the round-trip case prints.
func fillJSONM(t *testing.T, typ *runtime.DType) *runtime.DMsg {
	t.Helper()
	m := typ.New()
	mustSet(t, m.SetInt("big_id", math.MinInt64))
	mustSet(t, m.SetUint("huge", math.MaxUint64))
	mustSet(t, m.SetString("text", "hi \"yo\" \\ back\nnewline <tag> caf\u00e9"))
	mustSet(t, m.SetBytes("blob", []byte{0x00, 0x01, 0x02, 0xfa, 0xff}))
	mustSet(t, m.SetInt("color", 2))
	sub, err := m.NewMessage("sub")
	mustSet(t, err)
	mustSet(t, sub.SetString("name", "kid"))
	mustSet(t, sub.SetInt("value", 42))
	for _, v := range []int64{1, -2, 2147483647, -2147483648} {
		mustSet(t, m.SetInt("nums", v))
	}
	e1, err := m.NewMessage("scores")
	mustSet(t, err)
	mustSet(t, e1.SetString("key", "a"))
	mustSet(t, e1.SetInt("value", 1))
	e2, err := m.NewMessage("scores")
	mustSet(t, err)
	mustSet(t, e2.SetString("key", "b"))
	mustSet(t, e2.SetInt("value", 2))
	e3, err := m.NewMessage("by_id")
	mustSet(t, err)
	mustSet(t, e3.SetInt("key", 7))
	mustSet(t, e3.SetString("value", "seven"))
	mustSet(t, m.SetBool("flag", true))
	mustSet(t, m.SetDouble("ratio", 1.5))
	mustSet(t, m.SetFloat("quant", 2.5))
	tgt, err := m.NewMessage("target")
	mustSet(t, err)
	mustSet(t, tgt.SetString("name", "t"))
	mustSet(t, tgt.SetInt("value", 3))
	return m
}

const wantJSONM = "{\"bigId\":\"-9223372036854775808\",\"huge\":\"18446744073709551615\",\"text\":\"hi \\\"yo\\\" \\\\ back\\nnewline \\u003ctag\\u003e caf\u00e9\",\"blob\":\"AAEC+v8=\",\"color\":\"COLOR_GREEN\",\"sub\":{\"name\":\"kid\",\"value\":42},\"nums\":[1,-2,2147483647,-2147483648],\"scores\":{\"a\":1,\"b\":2},\"byId\":{\"7\":\"seven\"},\"flag\":true,\"ratio\":1.5,\"quant\":2.5,\"target\":{\"name\":\"t\",\"value\":3}}"

func TestJSONScalarsRoundTrip(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	m := fillJSONM(t, typ)
	got, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("MarshalJSON: %v", err)
	}
	if got != wantJSONM {
		t.Fatalf("print:\n got %s\nwant %s", got, wantJSONM)
	}
	back, err := typ.UnmarshalJSON(got, false)
	if err != nil {
		t.Fatalf("UnmarshalJSON: %v", err)
	}
	again, err := typ.MarshalJSON(back)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != wantJSONM {
		t.Fatalf("round trip:\n got %s\nwant %s", again, wantJSONM)
	}
	// Spot-check the decoded tree behind the text.
	if v, ok, err := back.GetInt("big_id"); err != nil || !ok || v != math.MinInt64 {
		t.Fatalf("big_id = %d ok=%v err=%v", v, ok, err)
	}
	if v, ok, err := back.GetUint("huge"); err != nil || !ok || v != math.MaxUint64 {
		t.Fatalf("huge = %d ok=%v err=%v", v, ok, err)
	}
	if v, ok, err := back.GetInt("color"); err != nil || !ok || v != 2 {
		t.Fatalf("color = %d ok=%v err=%v", v, ok, err)
	}
}

func TestJSONEnumNumbers(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	// Unknown number on an open (proto3) enum is kept as a number.
	m, err := typ.UnmarshalJSON(`{"color":99}`, false)
	if err != nil {
		t.Fatalf("open unknown number: %v", err)
	}
	if v, ok, err := m.GetInt("color"); err != nil || !ok || v != 99 {
		t.Fatalf("color = %d ok=%v err=%v", v, ok, err)
	}
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != `{"color":99}` {
		t.Fatalf("open unknown reprint = %s", out)
	}
	// Unknown number on a closed (proto2) enum lands in unknown bytes.
	req := mustType(t, s, "jsontest2.Req")
	rm, err := req.UnmarshalJSON(`{"need":1,"ec":99}`, false)
	if err != nil {
		t.Fatalf("closed unknown number: %v", err)
	}
	if ok, _ := rm.Has("ec"); ok {
		t.Fatalf("closed unknown number must not set the field")
	}
	if got := rm.UnknownBytes(); len(got) != 2 || got[0] != 0x10 || got[1] != 0x63 {
		t.Fatalf("unknown bytes = % x, want 10 63", got)
	}
	// Unknown names fail, and stay failed-but-skipped under ignore.
	if _, err := typ.UnmarshalJSON(`{"color":"NOPE"}`, false); err == nil {
		t.Fatalf("unknown enum name must fail")
	}
	skipped, err := typ.UnmarshalJSON(`{"color":"NOPE"}`, true)
	if err != nil {
		t.Fatalf("ignored unknown enum name: %v", err)
	}
	if ok, _ := skipped.Has("color"); ok {
		t.Fatalf("ignored unknown enum name must not set the field")
	}
	// Quoted numbers and names both spell the value.
	for _, text := range []string{`{"color":"COLOR_RED"}`, `{"color":"1"}`, `{"color":1}`} {
		got, err := typ.UnmarshalJSON(text, false)
		if err != nil {
			t.Fatalf("%s: %v", text, err)
		}
		if v, ok, err := got.GetInt("color"); err != nil || !ok || v != 1 {
			t.Fatalf("%s: color = %d ok=%v err=%v", text, v, ok, err)
		}
	}
}

func TestJSONMapPrintLastWins(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	m := typ.New()
	for _, kv := range [][2]any{{"a", int64(1)}, {"b", int64(2)}, {"a", int64(3)}} {
		e, err := m.NewMessage("scores")
		mustSet(t, err)
		mustSet(t, e.SetString("key", kv[0].(string)))
		mustSet(t, e.SetInt("value", kv[1].(int64)))
	}
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("MarshalJSON: %v", err)
	}
	if out != `{"scores":{"b":2,"a":3}}` {
		t.Fatalf("map last-wins print = %s", out)
	}
	// Duplicate keys fail the parse, like the reference.
	if _, err := typ.UnmarshalJSON(`{"scores":{"a":1,"a":2}}`, false); err == nil {
		t.Fatalf("duplicate map key must fail")
	}
}

func TestJSONWellKnown(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	m := typ.New()
	ts, err := m.NewMessage("ts")
	mustSet(t, err)
	mustSet(t, ts.SetInt("seconds", 0))
	mustSet(t, ts.SetInt("nanos", 1000000))
	dur, err := m.NewMessage("dur")
	mustSet(t, err)
	mustSet(t, dur.SetInt("seconds", 3))
	mustSet(t, dur.SetInt("nanos", 200000000))
	wi, err := m.NewMessage("wi")
	mustSet(t, err)
	mustSet(t, wi.SetInt("value", 7))
	fm, err := m.NewMessage("fm")
	mustSet(t, err)
	mustSet(t, fm.SetString("paths", "foo_bar"))
	mustSet(t, fm.SetString("paths", "baz"))
	e, err := m.NewMessage("e")
	mustSet(t, err)
	_ = e
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("MarshalJSON: %v", err)
	}
	want := `{"ts":"1970-01-01T00:00:00.001Z","dur":"3.200s","fm":"fooBar,baz","wi":7,"e":{}}`
	if out != want {
		t.Fatalf("wkt print:\n got %s\nwant %s", out, want)
	}
	back, err := typ.UnmarshalJSON(out, false)
	if err != nil {
		t.Fatalf("UnmarshalJSON: %v", err)
	}
	again, err := typ.MarshalJSON(back)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != want {
		t.Fatalf("wkt round trip:\n got %s\nwant %s", again, want)
	}
	// Object forms parse too.
	obj, err := typ.UnmarshalJSON(`{"ts":{"seconds":0,"nanos":1000000},"dur":"-3.2s","ws":"hi","lv":[1,"x",null],"v":1.5}`, false)
	if err != nil {
		t.Fatalf("wkt object forms: %v", err)
	}
	re, err := typ.MarshalJSON(obj)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if re != `{"ts":"1970-01-01T00:00:00.001Z","dur":"-3.200s","lv":[1,"x",null],"v":1.5,"ws":"hi"}` {
		t.Fatalf("wkt object round trip = %s", re)
	}
}

func TestJSONStruct(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	m, err := typ.UnmarshalJSON(`{"st":{"a":1.5,"b":"x","c":true,"d":null,"e":{"n":1},"f":[1,2]}}`, false)
	if err != nil {
		t.Fatalf("struct parse: %v", err)
	}
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("struct print: %v", err)
	}
	if out != `{"st":{"a":1.5,"b":"x","c":true,"d":null,"e":{"n":1},"f":[1,2]}}` {
		t.Fatalf("struct round trip = %s", out)
	}
	if _, err := typ.UnmarshalJSON(`{"st":{"a":1,"a":2}}`, false); err == nil {
		t.Fatalf("duplicate struct key must fail")
	}
}

func TestJSONAny(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.W")
	targ := mustType(t, s, "jsontest.Target")
	payload := targ.New()
	mustSet(t, payload.SetString("name", "packed"))
	mustSet(t, payload.SetInt("value", 9))
	wire, err := payload.Encode()
	mustSet(t, err)
	m := typ.New()
	any, err := m.NewMessage("any")
	mustSet(t, err)
	mustSet(t, any.SetString("type_url", "type.googleapis.com/jsontest.Target"))
	mustSet(t, any.SetBytes("value", wire))
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("MarshalJSON: %v", err)
	}
	want := `{"any":{"@type":"type.googleapis.com/jsontest.Target","name":"packed","value":9}}`
	if out != want {
		t.Fatalf("any print:\n got %s\nwant %s", out, want)
	}
	back, err := typ.UnmarshalJSON(out, false)
	if err != nil {
		t.Fatalf("any parse: %v", err)
	}
	again, err := typ.MarshalJSON(back)
	if err != nil {
		t.Fatalf("any reprint: %v", err)
	}
	if again != want {
		t.Fatalf("any round trip:\n got %s\nwant %s", again, want)
	}
	// Well-known payloads ride under "value".
	tsAny, err := typ.UnmarshalJSON(`{"any":{"@type":"type.googleapis.com/google.protobuf.Timestamp","value":"1970-01-01T00:00:00Z"}}`, false)
	if err != nil {
		t.Fatalf("any timestamp parse: %v", err)
	}
	re, err := typ.MarshalJSON(tsAny)
	if err != nil {
		t.Fatalf("any timestamp reprint: %v", err)
	}
	if re != `{"any":{"@type":"type.googleapis.com/google.protobuf.Timestamp","value":"1970-01-01T00:00:00Z"}}` {
		t.Fatalf("any timestamp round trip = %s", re)
	}
	// Unresolvable @type always fails, even when unknown fields are ignored.
	for _, ign := range []bool{false, true} {
		if _, err := typ.UnmarshalJSON(`{"any":{"@type":"type.googleapis.com/jsontest.Missing"}}`, ign); err == nil {
			t.Fatalf("unresolvable @type must fail (ignore=%v)", ign)
		}
	}
	// A bare @type-less object fails.
	if _, err := typ.UnmarshalJSON(`{"any":{"name":"x"}}`, false); err == nil {
		t.Fatalf("@type-less any must fail")
	}
}

func TestJSONUnknownAndRequired(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	if _, err := typ.UnmarshalJSON(`{"bogus":1}`, false); err == nil {
		t.Fatalf("unknown field must fail")
	}
	m, err := typ.UnmarshalJSON(`{"bogus":1,"flag":true}`, true)
	if err != nil {
		t.Fatalf("ignored unknown field: %v", err)
	}
	if v, ok, err := m.GetBool("flag"); err != nil || !ok || !v {
		t.Fatalf("flag = %v ok=%v err=%v", v, ok, err)
	}
	req := mustType(t, s, "jsontest2.Req")
	if _, err := req.UnmarshalJSON(`{"name":"x"}`, false); err == nil {
		t.Fatalf("missing required field must fail")
	}
	okm, err := req.UnmarshalJSON(`{"need":5}`, false)
	if err != nil {
		t.Fatalf("required present: %v", err)
	}
	if v, present, err := okm.GetInt("need"); err != nil || !present || v != 5 {
		t.Fatalf("need = %d present=%v err=%v", v, present, err)
	}
	// Unknown fails even with explicit presence next to it.
	if _, err := req.UnmarshalJSON(`{"need":1,"zzz":{}}`, true); err != nil {
		t.Fatalf("ignored unknown with required: %v", err)
	}
}

func TestJSONLegacyTolerances(t *testing.T) {
	s := loadJSONSchema(t)
	typ := mustType(t, s, "jsontest.M")
	m, err := typ.UnmarshalJSON(`{'bigId':'42','flag':'yes','ratio':1e3,'blob':'AAEC-v8',}`, false)
	if err != nil {
		t.Fatalf("legacy tolerances: %v", err)
	}
	if v, ok, _ := m.GetInt("big_id"); !ok || v != 42 {
		t.Fatalf("big_id = %d ok=%v", v, ok)
	}
	if v, ok, _ := m.GetBool("flag"); !ok || !v {
		t.Fatalf("flag = %v ok=%v", v, ok)
	}
	if v, ok, _ := m.GetDouble("ratio"); !ok || v != 1000 {
		t.Fatalf("ratio = %v ok=%v", v, ok)
	}
	if v, ok, _ := m.GetBytes("blob"); !ok || len(v) != 5 || v[3] != 0xfa {
		t.Fatalf("blob = % x ok=%v", v, ok)
	}
	// Bare keys, null as unset, nested-array flattening, implicit default drop.
	m2, err := typ.UnmarshalJSON(`{text:null,nums:[[1,2],3],flag:false}`, false)
	if err != nil {
		t.Fatalf("bare/null/flatten: %v", err)
	}
	out, err := typ.MarshalJSON(m2)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out != `{"nums":[1,2,3]}` {
		t.Fatalf("reprint = %s", out)
	}
}

func TestJSONOneofAndNames(t *testing.T) {
	raw := mustReadFile(t, "testdata/oneof1.desc")
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ := mustType(t, s, "spectest.Oneof1")
	// Proto and camelCase names both parse; print uses the JSON name.
	m, err := typ.UnmarshalJSON(`{"name":"n","num":5}`, false)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	out, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("print: %v", err)
	}
	if out != `{"name":"n","num":5}` {
		t.Fatalf("oneof print = %s", out)
	}
	// Two members of one oneof fail.
	if _, err := typ.UnmarshalJSON(`{"num":5,"text":"t"}`, false); err == nil {
		t.Fatalf("oneof duplicate must fail")
	}
	// Plain duplicates last-win and singular messages merge.
	m2, err := typ.UnmarshalJSON(`{"name":"a","name":"b","inner":{"v":1},"inner":{}}`, false)
	if err != nil {
		t.Fatalf("dup merge: %v", err)
	}
	out2, err := typ.MarshalJSON(m2)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if out2 != `{"name":"b","inner":{"v":1}}` {
		t.Fatalf("dup merge print = %s", out2)
	}
	// Unknown extension-style keys fail unless ignored.
	if _, err := typ.UnmarshalJSON(`{"[foo.bar]":1}`, false); err == nil {
		t.Fatalf("extension key must fail")
	}
}

func TestJSONMatrix3Fixture(t *testing.T) {
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
	text, err := typ.MarshalJSON(m)
	if err != nil {
		t.Fatalf("MarshalJSON: %v", err)
	}
	if text != `{"i32":-7,"str":"hello","color":"COLOR3_GREEN"}` {
		t.Fatalf("matrix3 print = %s", text)
	}
	back, err := typ.UnmarshalJSON(text, false)
	if err != nil {
		t.Fatalf("UnmarshalJSON: %v", err)
	}
	if v, ok, _ := back.GetInt("i32"); !ok || v != -7 {
		t.Fatalf("i32 = %d ok=%v", v, ok)
	}
	// Trailing garbage fails; empty object parses to the empty tree.
	if _, err := typ.UnmarshalJSON(text+` `+`{"a":1}`, false); err == nil {
		t.Fatalf("trailing bytes must fail")
	}
	empty, err := typ.UnmarshalJSON(`{}`, false)
	if err != nil {
		t.Fatalf("empty: %v", err)
	}
	again, err := typ.MarshalJSON(empty)
	if err != nil {
		t.Fatalf("reprint: %v", err)
	}
	if again != `{}` {
		t.Fatalf("empty print = %s", again)
	}
	// camelCase input lands on the underscore field.
	cm, err := typ.UnmarshalJSON(`{"optI32":-7}`, false)
	if err != nil {
		t.Fatalf("camelCase: %v", err)
	}
	if v, ok, _ := cm.GetInt("opt_i32"); !ok || v != -7 {
		t.Fatalf("opt_i32 = %d ok=%v", v, ok)
	}
	// Proto name prints as its JSON name.
	m2 := typ.New()
	mustSet(t, m2.SetInt("opt_i32", 1))
	o2, err := typ.MarshalJSON(m2)
	if err != nil {
		t.Fatalf("print: %v", err)
	}
	if !strings.Contains(o2, `"optI32":1`) {
		t.Fatalf("json name print = %s", o2)
	}
}
