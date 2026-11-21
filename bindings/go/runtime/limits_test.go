// Decode limits, required tracking and proto3 UTF-8 enforcement over the
// dynamic core. Mirrors the C core: pbw_limits in include/pbwire.h (zero
// means the defaults), limit checks in src/decode.c, required tracking in
// src/message.c and UTF-8 validation of proto3 strings on decode.
package runtime_test

import (
	"errors"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"

	"pbwire/runtime"
	"pbwire/tests/gen/matrix3"
)

// loadLimitSchema builds a recursive proto3 tree (Rec), a proto3 string
// holder (S), and proto2 required/optional shapes (Req/Sub/P2S).
func loadLimitSchema(t *testing.T) *runtime.Schema {
	t.Helper()
	opt := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	req := descriptorpb.FieldDescriptorProto_LABEL_REQUIRED
	ms := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE
	st := descriptorpb.FieldDescriptorProto_TYPE_STRING
	i32 := descriptorpb.FieldDescriptorProto_TYPE_INT32
	msg := func(name string, fields ...*descriptorpb.FieldDescriptorProto) *descriptorpb.DescriptorProto {
		return &descriptorpb.DescriptorProto{Name: jstrp(name), Field: fields}
	}
	rec := msg("Rec",
		jfield("child", 1, opt, ms, ".limtest.Rec"),
		jfield("name", 2, opt, st, ""))
	set := &descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{
		{
			Name:    jstrp("lim.proto"),
			Package: jstrp("limtest"),
			Syntax:  jstrp("proto3"),
			MessageType: []*descriptorpb.DescriptorProto{
				rec,
				msg("S", jfield("name", 1, opt, st, "")),
			},
		},
		{
			Name:    jstrp("lim2.proto"),
			Package: jstrp("limtest2"),
			Syntax:  jstrp("proto2"),
			MessageType: []*descriptorpb.DescriptorProto{
				msg("Req",
					jfield("need", 1, req, i32, ""),
					jfield("sub", 2, opt, ms, ".limtest2.Sub")),
				msg("Sub", jfield("x", 1, req, i32, "")),
				msg("P2S", jfield("name", 1, opt, st, "")),
			},
		},
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

// deepBinary nests field 1 (a message) levels deep around an empty core.
func deepBinary(levels int) []byte {
	body := []byte{}
	for i := 0; i < levels; i++ {
		frame := runtime.AppendTag(nil, 1, runtime.WireLen)
		frame = runtime.AppendVarint(frame, uint64(len(body)))
		body = append(frame, body...)
	}
	return body
}

func deepJSON(levels int) string {
	var sb strings.Builder
	for i := 0; i < levels; i++ {
		sb.WriteString(`{"child":`)
	}
	sb.WriteString(`{"name":"x"}`)
	for i := 0; i < levels; i++ {
		sb.WriteString(`}`)
	}
	return sb.String()
}

func deepText(levels int) string {
	var sb strings.Builder
	for i := 0; i < levels; i++ {
		sb.WriteString("child { ")
	}
	sb.WriteString(`name: "x"`)
	for i := 0; i < levels; i++ {
		sb.WriteString(" }")
	}
	return sb.String()
}

func TestLimitsMaxBytesBinary(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest.S")
	m := typ.New()
	mustSet(t, m.SetString("name", "hello"))
	wire, err := typ.Encode(m)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := typ.DecodeLimits(wire, runtime.Limits{MaxBytes: len(wire) - 1}); !errors.Is(err, runtime.ErrLimit) {
		t.Fatalf("over-cap decode: got %v, want ErrLimit", err)
	}
	back, err := typ.DecodeLimits(wire, runtime.Limits{MaxBytes: len(wire)})
	if err != nil {
		t.Fatalf("exact-cap decode: %v", err)
	}
	if v, ok, _ := back.GetString("name"); !ok || v != "hello" {
		t.Fatalf("name = %q ok=%v", v, ok)
	}
	// The zero value keeps the old behavior: no byte cap.
	if _, err := typ.DecodeLimits(wire, runtime.Limits{}); err != nil {
		t.Fatalf("zero limits: %v", err)
	}
}

func TestLimitsDepthBinary(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest.Rec")
	shallow := deepBinary(100)
	if _, err := typ.DecodeLimits(shallow, runtime.Limits{MaxDepth: 200}); err != nil {
		t.Fatalf("under-cap decode: %v", err)
	}
	if _, err := typ.DecodeLimits(shallow, runtime.Limits{MaxDepth: 50}); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("over-depth decode: got %v, want ErrDepth", err)
	}
	deep := deepBinary(300)
	if _, err := typ.Decode(deep); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("default deep decode: got %v, want ErrDepth", err)
	}
	// A cap above the hard maximum clamps instead of opening the gate.
	if _, err := typ.DecodeLimits(deep, runtime.Limits{MaxDepth: 1000}); !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("clamped deep decode: got %v, want ErrDepth", err)
	}
}

func TestLimitsDepthJSON(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest.Rec")
	shallow := deepJSON(100)
	if _, err := typ.UnmarshalJSONLimits(shallow, false, runtime.Limits{MaxDepth: 200}); err != nil {
		t.Fatalf("under-cap JSON: %v", err)
	}
	if _, err := typ.UnmarshalJSONLimits(shallow, false, runtime.Limits{MaxDepth: 50}); err == nil {
		t.Fatal("over-depth JSON must fail")
	}
	if _, err := typ.UnmarshalJSON(deepJSON(300), false); err == nil {
		t.Fatal("default deep JSON must fail")
	}
	if _, err := typ.UnmarshalJSONLimits(deepJSON(10), false, runtime.Limits{MaxBytes: 10}); !errors.Is(err, runtime.ErrLimit) {
		t.Fatalf("over-cap JSON: got %v, want ErrLimit", err)
	}
}

func TestLimitsDepthText(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest.Rec")
	shallow := deepText(100)
	if _, err := typ.UnmarshalTextLimits(shallow, runtime.Limits{MaxDepth: 200}); err != nil {
		t.Fatalf("under-cap text: %v", err)
	}
	if _, err := typ.UnmarshalTextLimits(shallow, runtime.Limits{MaxDepth: 50}); err == nil {
		t.Fatal("over-depth text must fail")
	}
	if _, err := typ.UnmarshalText(deepText(300)); err == nil {
		t.Fatal("default deep text must fail")
	}
	if _, err := typ.UnmarshalTextLimits(deepText(10), runtime.Limits{MaxBytes: 10}); !errors.Is(err, runtime.ErrLimit) {
		t.Fatalf("over-cap text: got %v, want ErrLimit", err)
	}
}

func TestLimitsDelimited(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest.Rec")
	shallow := deepBinary(100)
	frame := runtime.AppendVarint(nil, uint64(len(shallow)))
	frame = append(frame, shallow...)
	if _, n, err := typ.DecodeDelimitedLimits(frame, runtime.Limits{MaxDepth: 200}); err != nil || n != len(frame) {
		t.Fatalf("under-cap delimited: m=%v n=%d err=%v", n, len(frame), err)
	}
	if _, n, err := typ.DecodeDelimitedLimits(frame, runtime.Limits{MaxDepth: 50}); err == nil || n != 0 {
		t.Fatalf("over-depth delimited: n=%d err=%v", n, err)
	}
	if _, n, err := typ.DecodeDelimitedLimits(frame, runtime.Limits{MaxBytes: len(shallow) - 1}); !errors.Is(err, runtime.ErrLimit) || n != 0 {
		t.Fatalf("over-cap delimited: n=%d err=%v, want ErrLimit", n, err)
	}
	deep := deepBinary(300)
	prefix := runtime.AppendVarint(nil, uint64(len(deep)))
	deepFrame := append(prefix, deep...)
	if _, n, err := typ.DecodeDelimited(deepFrame); err == nil || n != 0 {
		t.Fatalf("default deep delimited: n=%d err=%v", n, err)
	}
	// The old entry points keep working through the defaults.
	m := typ.New()
	wire, err := typ.Encode(m)
	if err != nil {
		t.Fatal(err)
	}
	enc := append(runtime.AppendVarint(nil, uint64(len(wire))), wire...)
	if _, n, err := typ.DecodeDelimited(enc); err != nil || n != len(enc) {
		t.Fatalf("DecodeDelimited: n=%d err=%v", n, err)
	}
}

func TestRequiredIsInitialized(t *testing.T) {
	s := loadLimitSchema(t)
	typ := mustType(t, s, "limtest2.Req")
	empty := typ.New()
	if empty.IsInitialized() {
		t.Fatal("empty required tree must not be initialized")
	}
	mustSet(t, empty.SetInt("need", 1))
	if !empty.IsInitialized() {
		t.Fatal("present required field must be initialized")
	}
	withSub, err := func() (*runtime.DMsg, error) {
		m := typ.New()
		if err := m.SetInt("need", 1); err != nil {
			return nil, err
		}
		sub, err := m.NewMessage("sub")
		if err != nil {
			return nil, err
		}
		_ = sub
		return m, nil
	}()
	if err != nil {
		t.Fatal(err)
	}
	if withSub.IsInitialized() {
		t.Fatal("missing nested required field must not be initialized")
	}
	sub, err := withSub.NewMessage("sub")
	if err != nil {
		t.Fatal(err)
	}
	mustSet(t, sub.SetInt("x", 7))
	if !withSub.IsInitialized() {
		t.Fatal("complete required tree must be initialized")
	}
}

func TestProto3StringUTF8(t *testing.T) {
	s := loadLimitSchema(t)
	bad := runtime.AppendTag(nil, 1, runtime.WireLen)
	bad = append(bad, runtime.AppendVarint(nil, 2)...)
	bad = append(bad, 0xff, 0xfe)
	if _, err := mustType(t, s, "limtest.S").Decode(bad); err == nil {
		t.Fatal("invalid UTF-8 in a proto3 string must fail binary decode")
	}
	// Proto2 strings carry no such check, like the reference.
	if _, err := mustType(t, s, "limtest2.P2S").Decode(bad); err != nil {
		t.Fatalf("proto2 string must accept raw bytes: %v", err)
	}
}

func TestDecodeModelLimits(t *testing.T) {
	src := fillScalarsStruct(2)
	wire, err := runtime.EncodeModel(src, matrix3.Scalars3Meta)
	if err != nil {
		t.Fatal(err)
	}
	var back matrix3.Scalars3
	if err := runtime.DecodeModelLimits(wire, &back, matrix3.Scalars3Meta, runtime.Limits{}); err != nil {
		t.Fatalf("zero limits: %v", err)
	}
	if back.Str != "hello" {
		t.Fatalf("decode lost fields: %+v", back)
	}
	if err := runtime.DecodeModelLimits(wire, &back, matrix3.Scalars3Meta, runtime.Limits{MaxBytes: 3}); !errors.Is(err, runtime.ErrLimit) {
		t.Fatalf("over-cap model decode: got %v, want ErrLimit", err)
	}
}
