// Unknown-field retention through the shared table engine: hooks unset keep
// today's drop behavior; hooks set retain tag-inclusive bytes on decode and
// re-emit them after the known fields on encode.
package codec_test

import (
	"bytes"
	"testing"

	"pbwire/pbench/dyngo/codec"
)

type unkHolder struct {
	known []byte // retained unknown field bytes, tag included
	val   int32
}

func newUnkTable() *codec.Table {
	t := codec.NewTable("test.Unk", []*codec.Field{
		{Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32,
			GetI32: func(msg any) int32 { return msg.(*unkHolder).val },
			SetI32: func(msg any, v int32) { msg.(*unkHolder).val = v }},
	})
	t.Unknown = func(msg any, raw []byte) {
		msg.(*unkHolder).known = append(msg.(*unkHolder).known, raw...)
	}
	t.EncodeUnknown = func(msg any) []byte { return msg.(*unkHolder).known }
	return t
}

func TestUnknownRoundTrip(t *testing.T) {
	h := &unkHolder{}
	wire := []byte{0x08, 0x2a, 0x10, 0x01, 0x18, 0x02} // field 1 = 42, unknown 2, unknown 3
	d := codec.Decoder{Buf: wire}
	var a codec.Arena
	if err := codec.DecodeTable(&d, &a, h, newUnkTable(), 0); err != nil {
		t.Fatal(err)
	}
	if string(h.known) != string([]byte{0x10, 0x01, 0x18, 0x02}) {
		t.Fatalf("retained % x, want tag-inclusive 10 01 18 02", h.known)
	}
	var e codec.Encoder
	if err := codec.EncodeTable(&e, h, newUnkTable(), 0); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(e.Buf, wire) {
		t.Fatalf("re-encode % x, want % x", e.Buf, wire)
	}
}

func TestUnknownWrongWireType(t *testing.T) {
	h := &unkHolder{}
	wire := []byte{0x0d, 0x01, 0x00, 0x00, 0x00} // field 1 as fixed32: wrong wire type, must be retained
	d := codec.Decoder{Buf: wire}
	var a codec.Arena
	if err := codec.DecodeTable(&d, &a, h, newUnkTable(), 0); err != nil {
		t.Fatal(err)
	}
	if len(h.known) != 5 || h.val != 0 {
		t.Fatalf("known len %d val %d, want 5 and 0", len(h.known), h.val)
	}
}
