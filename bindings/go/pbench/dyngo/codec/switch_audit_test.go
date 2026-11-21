// Switch-audit guards: every shape the cshim path drives today must behave
// identically on the pure-Go table engine before generated models switch to
// it. A failing test here is a switch blocker, not a flaky oracle: each
// case pins byte output or a field-level round trip.
package codec_test

import (
	"bytes"
	"testing"

	"pbwire/pbench/dyngo/codec"
	"pbwire/tests/gen/maptest"
	"pbwire/tests/gen/oneof1"
)

// Proto2 required fields emit even at their zero value; an implicit field
// at zero stays silent. The engine keys this on Field.Required alone.
func TestAuditRequiredZeroEmits(t *testing.T) {
	type req struct {
		ReqInt int32
		ReqStr string
		ImpInt int32
	}
	tb := codec.NewTable("audit.Req", []*codec.Field{
		{Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32, Required: true,
			GetI32: func(msg any) int32 { return msg.(*req).ReqInt },
			SetI32: func(msg any, v int32) { msg.(*req).ReqInt = v }},
		{Num: 2, Wire: codec.WireLen, Kind: codec.KindString, Required: true,
			GetStr: func(msg any) string { return msg.(*req).ReqStr },
			SetStr: func(msg any, v string) { msg.(*req).ReqStr = v }},
		{Num: 3, Wire: codec.WireVarint, Kind: codec.KindInt32,
			GetI32: func(msg any) int32 { return msg.(*req).ImpInt },
			SetI32: func(msg any, v int32) { msg.(*req).ImpInt = v }},
	})
	wire, err := codec.EncodeModel(&req{}, tb)
	if err != nil {
		t.Fatal(err)
	}
	// field 1 varint 0, field 2 len 0; field 3 (implicit zero) absent.
	want := []byte{0x08, 0x00, 0x12, 0x00}
	if !bytes.Equal(wire, want) {
		t.Fatalf("required zero must emit: got %x want %x", wire, want)
	}
	back := new(req)
	if err := codec.DecodeModel(wire, back, tb); err != nil {
		t.Fatal(err)
	}
}

// Explicit bytes ride a pointer: nil skips, non-nil emits (even empty),
// and payloads round-trip.
func TestAuditExplicitBytes(t *testing.T) {
	type holder struct {
		Raw *[]byte
	}
	raw := []byte{0x01, 0x02}
	empty := []byte{}
	tb := codec.NewTable("audit.Raw", []*codec.Field{
		{Num: 1, Wire: codec.WireLen, Kind: codec.KindBytes, Explicit: true,
			GetOptBytes: func(msg any) *[]byte { return msg.(*holder).Raw },
			SetOptBytes: func(msg any, v []byte) {
				tmp := v
				msg.(*holder).Raw = &tmp
			}},
	})
	if wire, err := codec.EncodeModel(&holder{}, tb); err != nil || len(wire) != 0 {
		t.Fatalf("nil explicit bytes must skip: %x %v", wire, err)
	}
	wire, err := codec.EncodeModel(&holder{Raw: &empty}, tb)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, []byte{0x0a, 0x00}) {
		t.Fatalf("empty non-nil bytes must emit tag+len0: %x", wire)
	}
	wire, err = codec.EncodeModel(&holder{Raw: &raw}, tb)
	if err != nil {
		t.Fatal(err)
	}
	back := new(holder)
	if err := codec.DecodeModel(wire, back, tb); err != nil {
		t.Fatal(err)
	}
	if back.Raw == nil || !bytes.Equal(*back.Raw, raw) {
		t.Fatalf("bytes lost: %+v", back.Raw)
	}
}

// Recursive messages recurse through the row's lazy Sub table and
// terminate on nil.
func TestAuditRecursiveTable(t *testing.T) {
	type node struct {
		Val  int32
		Next *node
	}
	var tb *codec.Table
	tb = codec.NewTable("audit.Node", []*codec.Field{
		{Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32,
			GetI32: func(msg any) int32 { return msg.(*node).Val },
			SetI32: func(msg any, v int32) { msg.(*node).Val = v }},
		{Num: 2, Wire: codec.WireLen, Kind: codec.KindMessage,
			Sub: func() *codec.Table { return tb },
			GetMsg: func(msg any) any {
				if v := msg.(*node).Next; v == nil {
					return nil
				}
				return msg.(*node).Next
			},
			GetOrCreateMsg: func(msg any) any {
				m := msg.(*node)
				if m.Next == nil {
					m.Next = &node{}
				}
				return m.Next
			}},
	})
	in := &node{Val: 1, Next: &node{Val: 2, Next: &node{Val: 3}}}
	wire, err := codec.EncodeModel(in, tb)
	if err != nil {
		t.Fatal(err)
	}
	back := new(node)
	if err := codec.DecodeModel(wire, back, tb); err != nil {
		t.Fatal(err)
	}
	for i, want := range []int32{1, 2, 3} {
		if back == nil || back.Val != want {
			t.Fatalf("depth %d: got %+v want val %d", i, back, want)
		}
		back = back.Next
	}
	if back != nil {
		t.Fatalf("trailing node: %+v", back)
	}
}

// Generated oneof1 models: the table engine must match cshim bytes today,
// so the switch cannot change a single byte. Covers scalar, string and
// message members plus the empty-inner edge cshim encodes explicitly.
func TestAuditOneof1Parity(t *testing.T) {
	msgs := []*oneof1.Oneof1{
		{Name: "n", Kind: &oneof1.Oneof1_Num{Num: -42}},
		{Kind: &oneof1.Oneof1_Text{Text: "hi"}},
		{Name: "x", Kind: &oneof1.Oneof1_Inner_{Inner: &oneof1.Oneof1_Inner{V: -7}}},
		{Kind: &oneof1.Oneof1_Inner_{Inner: &oneof1.Oneof1_Inner{}}},
		{Name: "only"},
	}
	for i, m := range msgs {
		ref, err := m.Marshal()
		if err != nil {
			t.Fatalf("case %d: cshim marshal: %v", i, err)
		}
		got, err := codec.EncodeModel(m, oneof1.Oneof1Meta)
		if err != nil {
			t.Fatalf("case %d: table encode: %v", i, err)
		}
		if !bytes.Equal(got, ref) {
			t.Fatalf("case %d: table %x != cshim %x", i, got, ref)
		}
		back := new(oneof1.Oneof1)
		if err := codec.DecodeModel(ref, back, oneof1.Oneof1Meta); err != nil {
			t.Fatalf("case %d: table decode: %v", i, err)
		}
		again, err := codec.EncodeModel(back, oneof1.Oneof1Meta)
		if err != nil || !bytes.Equal(again, ref) {
			t.Fatalf("case %d: re-encode %x != %x (%v)", i, again, ref, err)
		}
	}
}

// Generated maptest models: repeated entry messages must match cshim bytes.
func TestAuditMaptestParity(t *testing.T) {
	m := &maptest.MapTest{
		ByInt: []*maptest.MapTest_ByIntEntry{{Key: -1, Value: "neg"}, {Key: 7, Value: "seven"}},
		ByStr: []*maptest.MapTest_ByStrEntry{{Key: "a", Value: -2}},
		ByMsg: []*maptest.MapTest_ByMsgEntry{{Key: "k", Value: &maptest.MapValue{Name: "v", Value: 3}}},
	}
	ref, err := m.Marshal()
	if err != nil {
		t.Fatalf("cshim marshal: %v", err)
	}
	got, err := codec.EncodeModel(m, maptest.MapTestMeta)
	if err != nil {
		t.Fatalf("table encode: %v", err)
	}
	if !bytes.Equal(got, ref) {
		t.Fatalf("table %x != cshim %x", got, ref)
	}
	back := new(maptest.MapTest)
	if err := codec.DecodeModel(ref, back, maptest.MapTestMeta); err != nil {
		t.Fatalf("table decode: %v", err)
	}
	again, err := codec.EncodeModel(back, maptest.MapTestMeta)
	if err != nil || !bytes.Equal(again, ref) {
		t.Fatalf("re-encode %x != %x (%v)", again, ref, err)
	}
}

// Unknown fields: both paths drop them today (no retention hooks on dev).
// Pin the parity so the switch cannot silently gain or lose retention.
func TestAuditUnknownDroppedBoth(t *testing.T) {
	base, err := codec.EncodeModel(&oneof1.Oneof1{Name: "n"}, oneof1.Oneof1Meta)
	if err != nil {
		t.Fatal(err)
	}
	wire := append(base, 0x98, 0x06, 0x07) // field 99, varint 7
	tabBack := new(oneof1.Oneof1)
	if err := codec.DecodeModel(wire, tabBack, oneof1.Oneof1Meta); err != nil {
		t.Fatalf("table decode: %v", err)
	}
	tabAgain, err := codec.EncodeModel(tabBack, oneof1.Oneof1Meta)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(tabAgain, base) {
		t.Fatalf("table path retained unknowns: %x", tabAgain)
	}
	cshimBack := new(oneof1.Oneof1)
	if err := cshimBack.Unmarshal(wire); err != nil {
		t.Fatalf("cshim decode: %v", err)
	}
	cshimAgain, err := cshimBack.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(cshimAgain, base) {
		t.Fatalf("cshim path retained unknowns: %x", cshimAgain)
	}
}
