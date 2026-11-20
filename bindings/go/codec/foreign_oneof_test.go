// A foreign oneof message member: the sibling package owns the wire work and
// the local engine only frames the bytes, the same shape as a foreign
// repeated message row.
package codec_test

import (
	"bytes"
	"testing"

	"pbwire/codec"
	dep "pbwire/tests/gen/xdep"
)

// The generated xfile model carries a plain cross-package message row; this
// pins the same Foreign framing for a oneof member through a hand-built row.
func TestForeignOneofMemberFraming(t *testing.T) {
	local := &dep.DepMsg{Name: "n", Value: 1}
	ref, err := local.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	d := codec.Decoder{Buf: ref}
	_ = d
	table := codec.NewTable("test.ForeignOneof", []*codec.Field{
		{
			Num: 1, Wire: codec.WireLen, Kind: codec.KindMessage, Oneof: true,
			Foreign: &codec.ForeignOps{
				Encode: func(m any) ([]byte, error) { return m.(*dep.DepMsg).Marshal() },
				Decode: func(m any, w []byte) error { return m.(*dep.DepMsg).Unmarshal(w) },
			},
			GetOneof: func(msg any) (any, bool) {
				h := msg.(*foreignHolder)
				if h.m == nil {
					return nil, false
				}
				return h.m, true
			},
			SetOneof: func(msg any, v any) {
				msg.(*foreignHolder).m = v.(*dep.DepMsg)
			},
			NewMsg: func() any { return &dep.DepMsg{} },
		},
	})
	h := &foreignHolder{m: local}
	var framed codec.Encoder
	framed.Tag(1, codec.WireLen)
	framed.Data(ref)
	var e codec.Encoder
	if err := codec.EncodeTable(&e, h, table, 0); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(e.Buf, framed.Buf) {
		t.Fatalf("encode % x, want % x", e.Buf, framed.Buf)
	}
	var back foreignHolder
	dd := codec.Decoder{Buf: e.Buf}
	var a codec.Arena
	if err := codec.DecodeTable(&dd, &a, &back, table, 0); err != nil {
		t.Fatal(err)
	}
	if back.m == nil || back.m.Name != "n" || back.m.Value != 1 {
		t.Fatalf("decode lost the member: %+v", back.m)
	}
}

type foreignHolder struct {
	m *dep.DepMsg
}
