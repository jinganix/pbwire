// Deterministic encode (map-entry key sort), exact encoded size, and
// length-delimited framing on the dynamic core.
package runtime_test

import (
	"bytes"
	"encoding/hex"
	"errors"
	"math"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"

	"pbwire/runtime"
	genmap "pbwire/tests/gen/maptest"
)

// cDetHex is the deterministic encoding of a spectest.MapTest holding
// by_int {(3,"three"),(-5,"neg"),(0,"zero")} and by_str
// {("c",3),("a",1),("b",2)} in that insertion order, produced by the C
// core's pbw_encode_deterministic (/tmp/detref.c over build/libpbwire.a):
// keys sorted (-5,0,3 and a,b,c), entry bytes otherwise identical.
const cDetHex = "0a1008fbffffffffffffffff0112036e65670a08080012047a65726f0a0908031205746872656512050a0161100112050a0162100212050a01631003"

func loadMapTest(t *testing.T) *runtime.DType {
	t.Helper()
	s, err := runtime.Load(mustReadFile(t, "testdata/maptest.desc"))
	if err != nil {
		t.Fatal(err)
	}
	return mustType(t, s, "spectest.MapTest")
}

// fillMapUnsorted builds the C-reference message: insertion order is
// deliberately unsorted on both map fields.
func fillMapUnsorted(t *testing.T, typ *runtime.DType) *runtime.DMsg {
	t.Helper()
	m := typ.New()
	intKeys := []int64{3, -5, 0}
	intVals := []string{"three", "neg", "zero"}
	for i := range intKeys {
		e, err := m.NewMessage("by_int")
		mustSet(t, err)
		mustSet(t, e.SetInt("key", intKeys[i]))
		mustSet(t, e.SetString("value", intVals[i]))
	}
	strKeys := []string{"c", "a", "b"}
	strVals := []int64{3, 1, 2}
	for i, k := range strKeys {
		e, err := m.NewMessage("by_str")
		mustSet(t, err)
		mustSet(t, e.SetString("key", k))
		mustSet(t, e.SetInt("value", strVals[i]))
	}
	return m
}

// entryKeys reads the key of every by_int entry; -1 marks a missing key.
func entryIntKeys(t *testing.T, m *runtime.DMsg) []int64 {
	t.Helper()
	run, err := m.GetMessageSlice("by_int")
	mustSet(t, err)
	out := make([]int64, len(run))
	for i, e := range run {
		v, ok, err := e.GetInt("key")
		mustSet(t, err)
		if !ok {
			v = -1
		}
		out[i] = v
	}
	return out
}

func entryStrKeys(t *testing.T, m *runtime.DMsg) []string {
	t.Helper()
	run, err := m.GetMessageSlice("by_str")
	mustSet(t, err)
	out := make([]string, len(run))
	for i, e := range run {
		v, _, err := e.GetString("key")
		mustSet(t, err)
		out[i] = v
	}
	return out
}

func TestDeterministicMapSort(t *testing.T) {
	typ := loadMapTest(t)
	m := fillMapUnsorted(t, typ)

	det1, err := typ.EncodeDeterministic(m)
	mustSet(t, err)

	// A second tree with the same pairs in a different insertion order
	// encodes identically.
	m2 := typ.New()
	for _, p := range [][2]any{{int64(0), "zero"}, {int64(3), "three"}, {int64(-5), "neg"}} {
		e, err := m2.NewMessage("by_int")
		mustSet(t, err)
		mustSet(t, e.SetInt("key", p[0].(int64)))
		mustSet(t, e.SetString("value", p[1].(string)))
	}
	for _, p := range [][2]any{{"b", int64(2)}, {"a", int64(1)}, {"c", int64(3)}} {
		e, err := m2.NewMessage("by_str")
		mustSet(t, err)
		mustSet(t, e.SetString("key", p[0].(string)))
		mustSet(t, e.SetInt("value", p[1].(int64)))
	}
	det2, err := typ.EncodeDeterministic(m2)
	mustSet(t, err)
	if !bytes.Equal(det1, det2) {
		t.Fatal("deterministic output depends on insertion order")
	}

	// Plain encode keeps insertion order, so it differs from sorted output.
	plain, err := typ.Encode(m)
	mustSet(t, err)
	if bytes.Equal(plain, det1) {
		t.Fatal("plain encode unexpectedly equals deterministic output")
	}

	back, err := typ.Decode(det1)
	mustSet(t, err)
	got := entryIntKeys(t, back)
	for i, want := range []int64{-5, 0, 3} {
		if got[i] != want {
			t.Fatalf("by_int order = %v, want [-5 0 3]", got)
		}
	}
	if strGot := entryStrKeys(t, back); len(strGot) != 3 || strGot[0] != "a" || strGot[1] != "b" || strGot[2] != "c" {
		t.Fatalf("by_str order = %v, want [a b c]", strGot)
	}
}

func TestDeterministicMatchesC(t *testing.T) {
	typ := loadMapTest(t)
	m := fillMapUnsorted(t, typ)
	det, err := typ.EncodeDeterministic(m)
	mustSet(t, err)
	want, err := hex.DecodeString(cDetHex)
	mustSet(t, err)
	if !bytes.Equal(det, want) {
		t.Fatalf("deterministic bytes differ from C:\n got %x\nwant %s", det, cDetHex)
	}
	// Sorted insertion through the plain encoder is byte-identical.
	sorted := typ.New()
	for i, k := range []int64{-5, 0, 3} {
		vals := []string{"neg", "zero", "three"}
		e, err := sorted.NewMessage("by_int")
		mustSet(t, err)
		mustSet(t, e.SetInt("key", k))
		mustSet(t, e.SetString("value", vals[i]))
	}
	for i, k := range []string{"a", "b", "c"} {
		e, err := sorted.NewMessage("by_str")
		mustSet(t, err)
		mustSet(t, e.SetString("key", k))
		mustSet(t, e.SetInt("value", int64(i+1)))
	}
	plain, err := typ.Encode(sorted)
	mustSet(t, err)
	if !bytes.Equal(plain, want) {
		t.Fatalf("sorted plain encode != C bytes:\n got %x\nwant %s", plain, cDetHex)
	}
}

func TestDeterministicMissingKeyFirst(t *testing.T) {
	typ := loadMapTest(t)
	m := typ.New()
	bare, err := m.NewMessage("by_int")
	mustSet(t, err)
	mustSet(t, bare.SetString("value", "nokey"))
	for _, k := range []int64{5, -3} {
		e, err := m.NewMessage("by_int")
		mustSet(t, err)
		mustSet(t, e.SetInt("key", k))
		mustSet(t, e.SetString("value", "v"))
	}
	det, err := typ.EncodeDeterministic(m)
	mustSet(t, err)
	back, err := typ.Decode(det)
	mustSet(t, err)
	run, err := back.GetMessageSlice("by_int")
	mustSet(t, err)
	if len(run) != 3 {
		t.Fatalf("got %d entries, want 3", len(run))
	}
	if _, ok, _ := run[0].GetInt("key"); ok {
		t.Fatal("first deterministic entry should be the keyless one")
	}
	k1, _, _ := run[1].GetInt("key")
	k2, _, _ := run[2].GetInt("key")
	if k1 != -3 || k2 != 5 {
		t.Fatalf("order after keyless = [%d %d], want [-3 5]", k1, k2)
	}
}

func TestDeterministicSortsCopy(t *testing.T) {
	typ := loadMapTest(t)
	m := fillMapUnsorted(t, typ)
	before := entryIntKeys(t, m)
	if _, err := typ.EncodeDeterministic(m); err != nil {
		t.Fatal(err)
	}
	after := entryIntKeys(t, m)
	if len(before) != len(after) {
		t.Fatalf("entry count changed: %v -> %v", before, after)
	}
	for i := range before {
		if before[i] != after[i] {
			t.Fatalf("caller slice reordered: %v -> %v", before, after)
		}
	}
}

func TestDeterministicKeyKinds(t *testing.T) {
	// Synthetic schema exercising the accessor-sensitive key kinds:
	// fixed32/uint64 compare by raw bits, sfixed32 numerically, and
	// bool keys order false before true.
	entry := func(name string, kt descriptorpb.FieldDescriptorProto_Type) *descriptorpb.DescriptorProto {
		return &descriptorpb.DescriptorProto{
			Name: jstrp(name),
			Field: []*descriptorpb.FieldDescriptorProto{
				jfield("key", 1, jlabelOpt, kt, ""),
				jfield("value", 2, jlabelOpt, descriptorpb.FieldDescriptorProto_TYPE_INT32, ""),
			},
			Options: &descriptorpb.MessageOptions{MapEntry: proto.Bool(true)},
		}
	}
	fd := &descriptorpb.FileDescriptorProto{
		Name:    jstrp("kinds.proto"),
		Package: jstrp("kindtest"),
		Syntax:  jstrp("proto3"),
		MessageType: []*descriptorpb.DescriptorProto{
			{
				Name: jstrp("K"),
				Field: []*descriptorpb.FieldDescriptorProto{
					jfield("fx", 1, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".kindtest.K.FxEntry"),
					jfield("sf", 2, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".kindtest.K.SfEntry"),
					jfield("ux", 3, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".kindtest.K.UxEntry"),
					jfield("bl", 4, jlabelRep, descriptorpb.FieldDescriptorProto_TYPE_MESSAGE, ".kindtest.K.BlEntry"),
				},
				NestedType: []*descriptorpb.DescriptorProto{
					entry("FxEntry", descriptorpb.FieldDescriptorProto_TYPE_FIXED32),
					entry("SfEntry", descriptorpb.FieldDescriptorProto_TYPE_SFIXED32),
					entry("UxEntry", descriptorpb.FieldDescriptorProto_TYPE_UINT64),
					entry("BlEntry", descriptorpb.FieldDescriptorProto_TYPE_BOOL),
				},
			},
		},
	}
	raw, err := proto.Marshal(&descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{fd}})
	mustSet(t, err)
	s, err := runtime.Load(raw)
	mustSet(t, err)
	typ := mustType(t, s, "kindtest.K")
	m := typ.New()
	for _, k := range []int64{0xFFFFFFFF, 1, 0x80000000} {
		e, err := m.NewMessage("fx")
		mustSet(t, err)
		mustSet(t, e.SetUint("key", uint64(k)))
		mustSet(t, e.SetInt("value", k&0xff))
	}
	for _, k := range []int64{5, -5, 0} {
		e, err := m.NewMessage("sf")
		mustSet(t, err)
		mustSet(t, e.SetInt("key", k))
		mustSet(t, e.SetInt("value", k))
	}
	for _, k := range []uint64{math.MaxUint64, 0, 7} {
		e, err := m.NewMessage("ux")
		mustSet(t, err)
		mustSet(t, e.SetUint("key", k))
	}
	for _, k := range []bool{true, false} {
		e, err := m.NewMessage("bl")
		mustSet(t, err)
		mustSet(t, e.SetBool("key", k))
	}
	det, err := typ.EncodeDeterministic(m)
	mustSet(t, err)
	back, err := typ.Decode(det)
	mustSet(t, err)
	intKeys := func(name string) []int64 {
		run, err := back.GetMessageSlice(name)
		mustSet(t, err)
		out := make([]int64, len(run))
		for i, e := range run {
			v, _, err := e.GetInt("key")
			mustSet(t, err)
			out[i] = v
		}
		return out
	}
	// fixed32/uint64 order by raw bits: 0, small, large.
	fx, err := back.GetMessageSlice("fx")
	mustSet(t, err)
	if len(fx) != 3 {
		t.Fatalf("fx entries = %d", len(fx))
	}
	f0, _, _ := fx[0].GetUint("key")
	f1, _, _ := fx[1].GetUint("key")
	f2, _, _ := fx[2].GetUint("key")
	if f0 != 1 || f1 != 0x80000000 || f2 != 0xFFFFFFFF {
		t.Fatalf("fixed32 order = [%d %d %d]", f0, f1, f2)
	}
	if got := intKeys("sf"); len(got) != 3 || got[0] != -5 || got[1] != 0 || got[2] != 5 {
		t.Fatalf("sfixed32 order = %v", got)
	}
	ux, err := back.GetMessageSlice("ux")
	mustSet(t, err)
	if len(ux) != 3 {
		t.Fatalf("ux entries = %d", len(ux))
	}
	u0, _, _ := ux[0].GetUint("key")
	u1, _, _ := ux[1].GetUint("key")
	u2, _, _ := ux[2].GetUint("key")
	if u0 != 0 || u1 != 7 || u2 != math.MaxUint64 {
		t.Fatalf("uint64 order = [%d %d %d]", u0, u1, u2)
	}
	bl, err := back.GetMessageSlice("bl")
	mustSet(t, err)
	b0, _, _ := bl[0].GetBool("key")
	b1, _, _ := bl[1].GetBool("key")
	if b0 || !b1 {
		t.Fatalf("bool order = [%v %v]", b0, b1)
	}
}

func TestDeterministicNonMapOrder(t *testing.T) {
	_, s := loadMatrix3(t)
	typ := mustType(t, s, "structtest.Scalars3")
	m := typ.New()
	for _, v := range []int64{3, 1, 2} {
		mustSet(t, m.SetInt("nums", v))
	}
	for _, v := range []string{"c", "a"} {
		mustSet(t, m.SetString("tags", v))
	}
	for i, name := range []string{"z", "m"} {
		e, err := m.NewMessage("inners")
		mustSet(t, err)
		mustSet(t, e.SetString("name", name))
		mustSet(t, e.SetInt("value", int64(i)))
	}
	plain, err := typ.Encode(m)
	mustSet(t, err)
	det, err := typ.EncodeDeterministic(m)
	mustSet(t, err)
	if !bytes.Equal(det, plain) {
		t.Fatal("message without maps: deterministic output must equal plain output")
	}
}

func TestEncodedSize(t *testing.T) {
	typ := loadMapTest(t)
	empty := typ.New()
	if empty.EncodedSize() != 0 {
		t.Fatalf("empty size = %d, want 0", empty.EncodedSize())
	}
	m := fillMapUnsorted(t, typ)
	nested, err := m.NewMessage("by_msg")
	mustSet(t, err)
	mustSet(t, nested.SetString("key", "k"))
	val, err := nested.NewMessage("value")
	mustSet(t, err)
	mustSet(t, val.SetString("name", "n"))
	mustSet(t, val.SetInt("value", -42))
	for _, tc := range []*runtime.DMsg{empty, m} {
		wire, err := tc.Encode()
		mustSet(t, err)
		if got := tc.EncodedSize(); got != len(wire) {
			t.Fatalf("EncodedSize = %d, len(Encode) = %d", got, len(wire))
		}
		det, err := typ.EncodeDeterministic(tc)
		mustSet(t, err)
		if got := tc.EncodedSize(); got != len(det) {
			t.Fatalf("EncodedSize = %d, len(EncodeDeterministic) = %d", got, len(det))
		}
	}
	// Broad scalar coverage: the JSON fixture spans int64-min, uint64-max,
	// strings, bytes, enums, nested, packed, string/int-keyed maps and
	// floats, plus unknown bytes retained from a tagged round trip.
	js := loadJSONSchema(t)
	jm := fillJSONM(t, mustType(t, js, "jsontest.M"))
	jwire, err := jm.Encode()
	mustSet(t, err)
	if got := jm.EncodedSize(); got != len(jwire) {
		t.Fatalf("json EncodedSize = %d, len(Encode) = %d", got, len(jwire))
	}
	jdet, err := mustType(t, js, "jsontest.M").EncodeDeterministic(jm)
	mustSet(t, err)
	if got := jm.EncodedSize(); got != len(jdet) {
		t.Fatalf("json EncodedSize = %d, len(EncodeDeterministic) = %d", got, len(jdet))
	}
	// Trees deeper than the cap report -1; Encode errors on the same tree.
	opt := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
	ms := descriptorpb.FieldDescriptorProto_TYPE_MESSAGE
	deepSet := &descriptorpb.FileDescriptorSet{File: []*descriptorpb.FileDescriptorProto{
		{
			Name:    jstrp("deep.proto"),
			Package: jstrp("deeptest"),
			Syntax:  jstrp("proto3"),
			MessageType: []*descriptorpb.DescriptorProto{
				{
					Name: jstrp("Rec"),
					Field: []*descriptorpb.FieldDescriptorProto{
						jfield("child", 1, opt, ms, ".deeptest.Rec"),
					},
				},
			},
		},
	}}
	raw, err := proto.Marshal(deepSet)
	mustSet(t, err)
	rs, err := runtime.Load(raw)
	mustSet(t, err)
	rtyp := mustType(t, rs, "deeptest.Rec")
	deep := rtyp.New()
	cur := deep
	for i := 0; i < runtime.MaxDepth+10; i++ {
		nxt, err := cur.NewMessage("child")
		mustSet(t, err)
		cur = nxt
	}
	if got := deep.EncodedSize(); got != -1 {
		t.Fatalf("deep EncodedSize = %d, want -1", got)
	}
	if _, err := deep.Encode(); err == nil {
		t.Fatal("deep Encode must fail past the depth cap")
	}
}

func TestFramingDelimited(t *testing.T) {
	typ := loadMapTest(t)
	m := fillMapUnsorted(t, typ)
	body, err := typ.Encode(m)
	mustSet(t, err)
	frame, err := m.EncodeDelimited()
	mustSet(t, err)
	if len(frame) == 0 || !bytes.Equal(frame[len(frame)-len(body):], body) {
		t.Fatal("EncodeDelimited must end with the plain body")
	}
	trailing := append(append([]byte(nil), frame...), 0x78, 0x79, 0x7a)
	back, consumed, err := typ.DecodeDelimited(trailing)
	mustSet(t, err)
	if consumed != len(frame) {
		t.Fatalf("consumed = %d, want %d", consumed, len(frame))
	}
	again, err := typ.Encode(back)
	mustSet(t, err)
	if !bytes.Equal(again, body) {
		t.Fatal("delimited round trip changed the message")
	}
	// Empty message: one zero byte, consumed exactly 1.
	zero, err := typ.New().EncodeDelimited()
	mustSet(t, err)
	if len(zero) != 1 || zero[0] != 0 {
		t.Fatalf("empty delimited = %x, want 00", zero)
	}
	if _, n, err := typ.DecodeDelimited(zero); err != nil || n != 1 {
		t.Fatalf("empty delimited decode: n=%d err=%v", n, err)
	}
	for name, wire := range map[string][]byte{
		"empty input":      {},
		"truncated varint": {0x80},
		"truncated body":   frame[:len(frame)-1],
		"length overrun":   {0x7f},
	} {
		if _, _, err := typ.DecodeDelimited(wire); !errors.Is(err, runtime.ErrWire) {
			t.Fatalf("%s: err = %v, want ErrWire", name, err)
		}
	}
}

func TestEncodeModelDeterministic(t *testing.T) {
	msg := &genmap.MapTest{
		ByInt: []*genmap.MapTest_ByIntEntry{
			{Key: 3, Value: "three"},
			{Key: -5, Value: "neg"},
			{Key: 0, Value: "zero"},
		},
		ByStr: []*genmap.MapTest_ByStrEntry{
			{Key: "c", Value: 3},
			{Key: "a", Value: 1},
			{Key: "b", Value: 2},
		},
	}
	det1, err := runtime.EncodeModelDeterministic(msg, genmap.MapTestMeta)
	mustSet(t, err)
	want, err := hex.DecodeString(cDetHex)
	mustSet(t, err)
	if !bytes.Equal(det1, want) {
		t.Fatalf("model deterministic != C:\n got %x\nwant %s", det1, cDetHex)
	}
	// Reordered insertion encodes identically; the caller slice is untouched.
	msg2 := &genmap.MapTest{
		ByInt: []*genmap.MapTest_ByIntEntry{
			{Key: 0, Value: "zero"},
			{Key: 3, Value: "three"},
			{Key: -5, Value: "neg"},
		},
		ByStr: []*genmap.MapTest_ByStrEntry{
			{Key: "b", Value: 2},
			{Key: "a", Value: 1},
			{Key: "c", Value: 3},
		},
	}
	det2, err := runtime.EncodeModelDeterministic(msg2, genmap.MapTestMeta)
	mustSet(t, err)
	if !bytes.Equal(det1, det2) {
		t.Fatal("model deterministic output depends on insertion order")
	}
	if msg.ByInt[0].Key != 3 || msg2.ByInt[0].Key != 0 {
		t.Fatal("deterministic encode reordered the caller slice")
	}
	// Plain marshal keeps insertion order.
	plain, err := msg.Marshal()
	mustSet(t, err)
	if bytes.Equal(plain, det1) {
		t.Fatal("plain marshal unexpectedly equals deterministic output")
	}
}
