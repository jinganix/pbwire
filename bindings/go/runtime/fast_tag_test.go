package runtime_test

// Characterization net for the fast-tag dispatch (1.2x bar): the 1-byte
// tag fast path must agree with the generic path on ordering, unknowns
// and truncation. Every closure panics, so any fallback to getters fails
// loudly; the 2-byte-tag field pins the slow lane.
import (
	"testing"
	"unsafe"

	pbwire "pbwire/runtime"
)

type fallbackProbe struct {
	A   int32    // field 1: 1-byte tag, fast lane
	B   int64    // field 2: 1-byte tag, fast lane
	Big int32    // field 20: 2-byte tag, slow lane
	RS  []string // field 3: repeated unpacked strings
	RN  []int32  // field 4: repeated packable numerics
	RK  []*fallbackKid
}

type fallbackKid struct{ V int32 }

func fallbackTable() *pbwire.Table {
	p := fallbackProbe{}
	mk := func(f *pbwire.Field) *pbwire.Field { f.Fast = true; return f }
	off := func(o uintptr) uint32 { return uint32(o) }
	return pbwire.NewTable("fallback", []*pbwire.Field{
		mk(&pbwire.Field{Num: 1, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Off: off(unsafe.Offsetof(p.A)), GetI32: fastPanic[int32](), SetI32: func(any, int32) { panic("x") }}),
		mk(&pbwire.Field{Num: 2, Wire: pbwire.WireVarint, Kind: pbwire.KindInt64, Off: off(unsafe.Offsetof(p.B)), GetI64: fastPanic[int64](), SetI64: func(any, int64) { panic("x") }}),
		mk(&pbwire.Field{Num: 20, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Off: off(unsafe.Offsetof(p.Big)), GetI32: fastPanic[int32](), SetI32: func(any, int32) { panic("x") }}),
		mk(&pbwire.Field{Num: 3, Wire: pbwire.WireLen, Kind: pbwire.KindString, Repeated: true, Off: off(unsafe.Offsetof(p.RS)), GetRepStr: func(m any) []string { return m.(*fallbackProbe).RS }, AppendStr: func(m any, v string) { p := m.(*fallbackProbe); p.RS = append(p.RS, v) }}),
		mk(&pbwire.Field{Num: 4, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Repeated: true, Packed: true, Off: off(unsafe.Offsetof(p.RN)), GetRepI32: func(m any) []int32 { return m.(*fallbackProbe).RN }, AppendI32: func(m any, v int32) { p := m.(*fallbackProbe); p.RN = append(p.RN, v) }}),
		mk(&pbwire.Field{Num: 5, Wire: pbwire.WireLen, Kind: pbwire.KindMessage, Repeated: true, Off: off(unsafe.Offsetof(p.RK)),
			Sub: func() *pbwire.Table {
				return pbwire.NewTable("fallbackkid", []*pbwire.Field{{
					Num: 1, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32,
					GetI32: func(m any) int32 { return m.(*fallbackKid).V },
					SetI32: func(m any, v int32) { m.(*fallbackKid).V = v },
				}})
			},
			NewRepMsg: func(m any) any {
				k := &fallbackKid{}
				p := m.(*fallbackProbe)
				p.RK = append(p.RK, k)
				return k
			}}),
	})
}

func fallbackWire(t *testing.T, order string) []byte {
	t.Helper()
	var b []byte
	put := func(num uint32, v uint64) {
		b = pbwire.AppendTag(b, num, pbwire.WireVarint)
		b = pbwire.AppendVarint(b, v)
	}
	unknown := func() {
		b = pbwire.AppendTag(b, 30, pbwire.WireVarint)
		b = pbwire.AppendVarint(b, 7)
	}
	str := func(num uint32, v string) {
		b = pbwire.AppendTag(b, num, pbwire.WireLen)
		b = pbwire.AppendVarint(b, uint64(len(v)))
		b = append(b, v...)
	}
	packed := func(num uint32, vs []int32) {
		b = pbwire.AppendTag(b, num, pbwire.WireLen)
		var run []byte
		for _, v := range vs {
			run = pbwire.AppendVarint(run, uint64(int64(v)))
		}
		b = pbwire.AppendVarint(b, uint64(len(run)))
		b = append(b, run...)
	}
	submsg := func(num uint32, v int32) {
		var inner []byte
		inner = pbwire.AppendTag(inner, 1, pbwire.WireVarint)
		inner = pbwire.AppendVarint(inner, uint64(int64(v)))
		b = pbwire.AppendTag(b, num, pbwire.WireLen)
		b = pbwire.AppendVarint(b, uint64(len(inner)))
		b = append(b, inner...)
	}
	switch order {
	case "ordered":
		put(1, 300)
		put(2, 70000)
		put(20, 5)
		str(3, "x")
		str(3, "yz")
		packed(4, []int32{1, 2, 300})
		submsg(5, 9)
		submsg(5, 10)
	case "shuffled":
		submsg(5, 9)
		unknown()
		put(2, 70000)
		str(3, "x")
		put(20, 5)
		packed(4, []int32{1, 2, 300})
		str(3, "yz")
		submsg(5, 10)
		put(1, 300)
	default:
		t.Fatalf("unknown order %q", order)
	}
	return b
}

func decodeFallback(t *testing.T, wire []byte) (*fallbackProbe, error) {
	t.Helper()
	d := &pbwire.Decoder{Buf: wire}
	a := &pbwire.Arena{}
	got := &fallbackProbe{}
	err := pbwire.DecodeTable(d, a, got, fallbackTable(), 0)
	return got, err
}

// TestFastTagFallbackParity pins the dispatch contract the fast-tag lane
// must honor: field order on the wire is irrelevant, unknown fields ride
// along dropped, 2-byte tags stay generic, and truncation still fails.
func TestFastTagFallbackParity(t *testing.T) {
	for _, order := range []string{"ordered", "shuffled"} {
		got, err := decodeFallback(t, fallbackWire(t, order))
		if err != nil {
			t.Fatalf("%s: %v", order, err)
		}
		if got.A != 300 || got.B != 70000 || got.Big != 5 ||
			len(got.RS) != 2 || got.RS[0] != "x" || got.RS[1] != "yz" ||
			len(got.RN) != 3 || got.RN[0] != 1 || got.RN[2] != 300 ||
			len(got.RK) != 2 || got.RK[0].V != 9 || got.RK[1].V != 10 {
			t.Fatalf("%s: got %+v", order, got)
		}
	}
	// Truncated tail: the last value varint loses its final byte.
	full := fallbackWire(t, "ordered")
	if _, err := decodeFallback(t, full[:len(full)-1]); err == nil {
		t.Fatal("truncated tail decoded without error, want ErrWire")
	}
}
