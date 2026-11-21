// Switch-audit guards: every shape the retired cshim path drove must behave
// identically on the pure-Go table engine that generated models now use.
// Each case pins byte output or a field-level round trip.
package runtime_test

import (
	"bytes"
	"testing"

	"pbwire/runtime"
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
	tb := runtime.NewTable("audit.Req", []*runtime.Field{
		{Num: 1, Wire: runtime.WireVarint, Kind: runtime.KindInt32, Required: true,
			GetI32: func(msg any) int32 { return msg.(*req).ReqInt },
			SetI32: func(msg any, v int32) { msg.(*req).ReqInt = v }},
		{Num: 2, Wire: runtime.WireLen, Kind: runtime.KindString, Required: true,
			GetStr: func(msg any) string { return msg.(*req).ReqStr },
			SetStr: func(msg any, v string) { msg.(*req).ReqStr = v }},
		{Num: 3, Wire: runtime.WireVarint, Kind: runtime.KindInt32,
			GetI32: func(msg any) int32 { return msg.(*req).ImpInt },
			SetI32: func(msg any, v int32) { msg.(*req).ImpInt = v }},
	})
	wire, err := runtime.EncodeModel(&req{}, tb)
	if err != nil {
		t.Fatal(err)
	}
	// field 1 varint 0, field 2 len 0; field 3 (implicit zero) absent.
	want := []byte{0x08, 0x00, 0x12, 0x00}
	if !bytes.Equal(wire, want) {
		t.Fatalf("required zero must emit: got %x want %x", wire, want)
	}
	back := new(req)
	if err := runtime.DecodeModel(wire, back, tb); err != nil {
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
	tb := runtime.NewTable("audit.Raw", []*runtime.Field{
		{Num: 1, Wire: runtime.WireLen, Kind: runtime.KindBytes, Explicit: true,
			GetOptBytes: func(msg any) *[]byte { return msg.(*holder).Raw },
			SetOptBytes: func(msg any, v []byte) {
				tmp := v
				msg.(*holder).Raw = &tmp
			}},
	})
	if wire, err := runtime.EncodeModel(&holder{}, tb); err != nil || len(wire) != 0 {
		t.Fatalf("nil explicit bytes must skip: %x %v", wire, err)
	}
	wire, err := runtime.EncodeModel(&holder{Raw: &empty}, tb)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, []byte{0x0a, 0x00}) {
		t.Fatalf("empty non-nil bytes must emit tag+len0: %x", wire)
	}
	wire, err = runtime.EncodeModel(&holder{Raw: &raw}, tb)
	if err != nil {
		t.Fatal(err)
	}
	back := new(holder)
	if err := runtime.DecodeModel(wire, back, tb); err != nil {
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
	var tb *runtime.Table
	tb = runtime.NewTable("audit.Node", []*runtime.Field{
		{Num: 1, Wire: runtime.WireVarint, Kind: runtime.KindInt32,
			GetI32: func(msg any) int32 { return msg.(*node).Val },
			SetI32: func(msg any, v int32) { msg.(*node).Val = v }},
		{Num: 2, Wire: runtime.WireLen, Kind: runtime.KindMessage,
			Sub: func() *runtime.Table { return tb },
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
	wire, err := runtime.EncodeModel(in, tb)
	if err != nil {
		t.Fatal(err)
	}
	back := new(node)
	if err := runtime.DecodeModel(wire, back, tb); err != nil {
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

// Generated oneof1 models round-trip through the table engine, including
// scalar, string and message members plus the empty-inner edge that presence
// in the wrapper (not the value) keeps alive.
func TestAuditOneof1RoundTrip(t *testing.T) {
	msgs := []*oneof1.Oneof1{
		{Name: "n", Kind: &oneof1.Oneof1_Num{Num: -42}},
		{Kind: &oneof1.Oneof1_Text{Text: "hi"}},
		{Name: "x", Kind: &oneof1.Oneof1_Inner_{Inner: &oneof1.Oneof1_Inner{V: -7}}},
		{Kind: &oneof1.Oneof1_Inner_{Inner: &oneof1.Oneof1_Inner{}}},
		{Name: "only"},
	}
	for i, m := range msgs {
		wire, err := runtime.EncodeModel(m, oneof1.Oneof1Meta)
		if err != nil {
			t.Fatalf("case %d: encode: %v", i, err)
		}
		back := new(oneof1.Oneof1)
		if err := runtime.DecodeModel(wire, back, oneof1.Oneof1Meta); err != nil {
			t.Fatalf("case %d: decode: %v", i, err)
		}
		again, err := runtime.EncodeModel(back, oneof1.Oneof1Meta)
		if err != nil || !bytes.Equal(again, wire) {
			t.Fatalf("case %d: re-encode %x != %x (%v)", i, again, wire, err)
		}
		if back.Name != m.Name {
			t.Fatalf("case %d: name %q want %q", i, back.Name, m.Name)
		}
		switch w := m.Kind.(type) {
		case *oneof1.Oneof1_Num:
			g, ok := back.Kind.(*oneof1.Oneof1_Num)
			if !ok || g.Num != w.Num {
				t.Fatalf("case %d: kind mismatch %+v", i, back.Kind)
			}
		case *oneof1.Oneof1_Text:
			g, ok := back.Kind.(*oneof1.Oneof1_Text)
			if !ok || g.Text != w.Text {
				t.Fatalf("case %d: kind mismatch %+v", i, back.Kind)
			}
		case *oneof1.Oneof1_Inner_:
			g, ok := back.Kind.(*oneof1.Oneof1_Inner_)
			if !ok || (g.Inner == nil) != (w.Inner == nil) || (w.Inner != nil && g.Inner.V != w.Inner.V) {
				t.Fatalf("case %d: kind mismatch %+v", i, back.Kind)
			}
		default:
			if back.Kind != nil {
				t.Fatalf("case %d: kind %+v want nil", i, back.Kind)
			}
		}
	}
}

// Generated maptest models round-trip through the table engine: repeated
// entry messages with string, int and message values.
func TestAuditMaptestRoundTrip(t *testing.T) {
	m := &maptest.MapTest{
		ByInt: []*maptest.MapTest_ByIntEntry{{Key: -1, Value: "neg"}, {Key: 7, Value: "seven"}},
		ByStr: []*maptest.MapTest_ByStrEntry{{Key: "a", Value: -2}},
		ByMsg: []*maptest.MapTest_ByMsgEntry{{Key: "k", Value: &maptest.MapValue{Name: "v", Value: 3}}},
	}
	wire, err := runtime.EncodeModel(m, maptest.MapTestMeta)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	back := new(maptest.MapTest)
	if err := runtime.DecodeModel(wire, back, maptest.MapTestMeta); err != nil {
		t.Fatalf("decode: %v", err)
	}
	again, err := runtime.EncodeModel(back, maptest.MapTestMeta)
	if err != nil || !bytes.Equal(again, wire) {
		t.Fatalf("re-encode %x != %x (%v)", again, wire, err)
	}
	if len(back.ByInt) != 2 || len(back.ByStr) != 1 || len(back.ByMsg) != 1 {
		t.Fatalf("entries lost: %+v", back)
	}
	if back.ByMsg[0].Value.Name != "v" || back.ByInt[0].Value != "neg" {
		t.Fatalf("entry payloads lost: %+v", back)
	}
}

// Unknown fields are dropped on the table path (no retention hooks): pin
// the drop so a future retention change is a deliberate, tested decision.
func TestAuditUnknownDropped(t *testing.T) {
	base, err := runtime.EncodeModel(&oneof1.Oneof1{Name: "n"}, oneof1.Oneof1Meta)
	if err != nil {
		t.Fatal(err)
	}
	wire := append(base, 0x98, 0x06, 0x07) // field 99, varint 7
	back := new(oneof1.Oneof1)
	if err := runtime.DecodeModel(wire, back, oneof1.Oneof1Meta); err != nil {
		t.Fatalf("decode: %v", err)
	}
	again, err := runtime.EncodeModel(back, oneof1.Oneof1Meta)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, base) {
		t.Fatalf("unknowns retained: %x", again)
	}
}
