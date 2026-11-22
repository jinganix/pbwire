// White-box pin for the 2-byte tag fast path: NewTable must resolve the
// 2-byte tags matrix3 actually carries (16..22, unpacked and packed forms)
// to their rows, and must leave colliding first bytes empty so those tags
// stay on the Next+lookup path. Fails to build before the fastTag2 columns
// exist; fails after if the install rule drifts.
package runtime

import "testing"

func TestFastTag2Build(t *testing.T) {
	mk := func(num uint32, wire WireType, kind Kind, rep bool) *Field {
		return &Field{Num: num, Wire: wire, Kind: kind, Repeated: rep, Fast: true}
	}
	tb := NewTable("t2build", []*Field{
		mk(16, WireLen, KindMessage, false),  // 130: 0x82 0x01
		mk(17, WireVarint, KindInt32, true),  // 136: 0x88 0x01 + packed 138: 0x8A 0x01
		mk(18, WireLen, KindString, true),    // 146: 0x92 0x01
		mk(19, WireLen, KindMessage, true),   // 154: 0x9A 0x01
		mk(20, WireVarint, KindInt32, false), // 160: 0xA0 0x01
		mk(21, WireLen, KindString, false),   // 170: 0xAA 0x01
		mk(22, WireVarint, KindBool, true),   // 176: 0xB0 0x01 + packed 178: 0xB2 0x01
	})
	want := map[byte]uint32{
		0x82: 16, 0x88: 17, 0x8A: 17, 0x92: 18, 0x9A: 19,
		0xA0: 20, 0xAA: 21, 0xB0: 22, 0xB2: 22,
	}
	for b0, num := range want {
		f := tb.fastTag2[b0]
		if f == nil {
			t.Fatalf("fastTag2[%#02x] empty, want field %d", b0, num)
		}
		if f.Num != num {
			t.Fatalf("fastTag2[%#02x] = field %d, want %d", b0, f.Num, num)
		}
	}
	wantB1 := map[byte]byte{
		0x82: 0x01, 0x88: 0x01, 0x8A: 0x01, 0x92: 0x01, 0x9A: 0x01,
		0xA0: 0x01, 0xAA: 0x01, 0xB0: 0x01, 0xB2: 0x01,
	}
	for b0, b1 := range wantB1 {
		if tb.fastTag2b[b0] != b1 {
			t.Fatalf("fastTag2b[%#02x] = %#02x, want %#02x", b0, tb.fastTag2b[b0], b1)
		}
	}
}

func TestFastTag2CollisionStaysGeneric(t *testing.T) {
	mk := func(num uint32) *Field {
		return &Field{Num: num, Wire: WireVarint, Kind: KindInt32, Fast: true}
	}
	// Field 17 unpacked is tag 136 (0x88 0x01); field 33 unpacked is tag
	// 264 (0x88 0x02): same first byte, different second. Both must stay
	// off the fast path so neither mis-dispatches to the other.
	tb := NewTable("t2collide", []*Field{mk(17), mk(33)})
	if tb.fastTag2[0x88] != nil {
		t.Fatalf("fastTag2[0x88] = field %d on collision, want nil (generic path)",
			tb.fastTag2[0x88].Num)
	}
}
