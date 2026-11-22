// Behavioral net for the 2-byte tag fast path: every 2-byte tag form the
// engine may fast-dispatch must decode exactly like the generic path —
// mixed packed/unpacked runs, singular and repeated messages, explicit
// optionals, unknown-field retention (pins the lastTag update the fast
// resolve must do), first-byte collisions (stay generic, never
// mis-dispatch) and truncation (error, never panic).
package runtime_test

import (
	"bytes"
	"testing"
	"unsafe"

	pbwire "pbwire/runtime"
)

type tag2Kid struct{ V int32 }

type tag2Probe struct {
	Inner *tag2Kid
	Nums  []int32
	Tags  []string
	Kids  []*tag2Kid
	Opt   *int32
	Flags []bool
}

func tag2Table() *pbwire.Table {
	p := tag2Probe{}
	mk := func(f *pbwire.Field) *pbwire.Field { f.Fast = true; return f }
	off := func(o uintptr) uint32 { return uint32(o) }
	kidTab := func() *pbwire.Table {
		return pbwire.NewTable("tag2kid", []*pbwire.Field{{
			Num: 1, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32,
			GetI32: func(m any) int32 { return m.(*tag2Kid).V },
			SetI32: func(m any, v int32) { m.(*tag2Kid).V = v },
		}})
	}
	return pbwire.NewTable("tag2", []*pbwire.Field{
		mk(&pbwire.Field{Num: 16, Wire: pbwire.WireLen, Kind: pbwire.KindMessage, Off: off(unsafe.Offsetof(p.Inner)),
			Sub: kidTab, GetMsg: func(m any) any { return m.(*tag2Probe).Inner },
			GetOrCreateMsg: func(m any) any {
				q := m.(*tag2Probe)
				if q.Inner == nil {
					q.Inner = &tag2Kid{}
				}
				return q.Inner
			}}),
		mk(&pbwire.Field{Num: 17, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Repeated: true, Packed: true, Off: off(unsafe.Offsetof(p.Nums)),
			GetRepI32: func(m any) []int32 { return m.(*tag2Probe).Nums },
			AppendI32: func(m any, v int32) { q := m.(*tag2Probe); q.Nums = append(q.Nums, v) }}),
		mk(&pbwire.Field{Num: 18, Wire: pbwire.WireLen, Kind: pbwire.KindString, Repeated: true, Off: off(unsafe.Offsetof(p.Tags)),
			GetRepStr: func(m any) []string { return m.(*tag2Probe).Tags },
			AppendStr: func(m any, v string) { q := m.(*tag2Probe); q.Tags = append(q.Tags, v) }}),
		mk(&pbwire.Field{Num: 19, Wire: pbwire.WireLen, Kind: pbwire.KindMessage, Repeated: true, Off: off(unsafe.Offsetof(p.Kids)),
			Sub: kidTab,
			NewRepMsg: func(m any) any {
				k := &tag2Kid{}
				q := m.(*tag2Probe)
				q.Kids = append(q.Kids, k)
				return k
			}}),
		mk(&pbwire.Field{Num: 20, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Explicit: true, Off: off(unsafe.Offsetof(p.Opt)), Has: off(unsafe.Offsetof(p.Opt)),
			GetOptI32: func(m any) *int32 { return m.(*tag2Probe).Opt },
			SetOptI32: func(m any, v int32) { q := m.(*tag2Probe); q.Opt = &v }}),
		mk(&pbwire.Field{Num: 22, Wire: pbwire.WireVarint, Kind: pbwire.KindBool, Repeated: true, Packed: true, Off: off(unsafe.Offsetof(p.Flags)),
			GetRepBool: func(m any) []bool { return m.(*tag2Probe).Flags },
			AppendBool: func(m any, v bool) { q := m.(*tag2Probe); q.Flags = append(q.Flags, v) }}),
	})
}

func tag2Wire(t *testing.T) []byte {
	t.Helper()
	var b []byte
	// Singular message field 16.
	var inner []byte
	inner = pbwire.AppendTag(inner, 1, pbwire.WireVarint)
	inner = pbwire.AppendVarint(inner, 42)
	b = pbwire.AppendTag(b, 16, pbwire.WireLen)
	b = pbwire.AppendVarint(b, uint64(len(inner)))
	b = append(b, inner...)
	// Field 17 unpacked then packed in the same payload.
	b = pbwire.AppendTag(b, 17, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 7)
	var run []byte
	for _, v := range []int32{-3, 300} {
		run = pbwire.AppendVarint(run, uint64(int64(v)))
	}
	b = pbwire.AppendTag(b, 17, pbwire.WireLen)
	b = pbwire.AppendVarint(b, uint64(len(run)))
	b = append(b, run...)
	// Repeated strings, repeated messages, explicit optional.
	for _, s := range []string{"tag", "tag2"} {
		b = pbwire.AppendTag(b, 18, pbwire.WireLen)
		b = pbwire.AppendVarint(b, uint64(len(s)))
		b = append(b, s...)
	}
	for _, v := range []int32{1, 2} {
		var k []byte
		k = pbwire.AppendTag(k, 1, pbwire.WireVarint)
		k = pbwire.AppendVarint(k, uint64(int64(v)))
		b = pbwire.AppendTag(b, 19, pbwire.WireLen)
		b = pbwire.AppendVarint(b, uint64(len(k)))
		b = append(b, k...)
	}
	b = pbwire.AppendTag(b, 20, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 9)
	// Field 22 unpacked then packed.
	b = pbwire.AppendTag(b, 22, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 1)
	var brun []byte
	brun = pbwire.AppendVarint(brun, 0)
	brun = pbwire.AppendVarint(brun, 1)
	b = pbwire.AppendTag(b, 22, pbwire.WireLen)
	b = pbwire.AppendVarint(b, uint64(len(brun)))
	b = append(b, brun...)
	return b
}

func decodeTag2(t *testing.T, wire []byte, hook func(any, []byte)) *tag2Probe {
	t.Helper()
	d := &pbwire.Decoder{Buf: wire}
	a := &pbwire.Arena{}
	got := &tag2Probe{}
	tab := tag2Table()
	if hook != nil {
		tab.Unknown = hook
	}
	if err := pbwire.DecodeTable(d, a, got, tab, 0); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return got
}

// TestFastTag2Parity pins mixed 2-byte traffic: packed and unpacked runs
// of the same field, singular and repeated messages, and the explicit
// optional must all land exactly.
func TestFastTag2Parity(t *testing.T) {
	got := decodeTag2(t, tag2Wire(t), nil)
	if got.Inner == nil || got.Inner.V != 42 {
		t.Fatalf("inner = %+v, want {42}", got.Inner)
	}
	if len(got.Nums) != 3 || got.Nums[0] != 7 || got.Nums[1] != -3 || got.Nums[2] != 300 {
		t.Fatalf("nums = %v", got.Nums)
	}
	if len(got.Tags) != 2 || got.Tags[0] != "tag" || got.Tags[1] != "tag2" {
		t.Fatalf("tags = %v", got.Tags)
	}
	if len(got.Kids) != 2 || got.Kids[0].V != 1 || got.Kids[1].V != 2 {
		t.Fatalf("kids = %+v", got.Kids)
	}
	if got.Opt == nil || *got.Opt != 9 {
		t.Fatalf("opt = %+v, want 9", got.Opt)
	}
	if len(got.Flags) != 3 || !got.Flags[0] || got.Flags[1] || !got.Flags[2] {
		t.Fatalf("flags = %v", got.Flags)
	}
}

// TestFastTag2UnknownRetention pins lastTag on the fast resolve: an unknown
// 2-byte tag must be retained with its tag bytes intact.
func TestFastTag2UnknownRetention(t *testing.T) {
	wire := tag2Wire(t)
	wire = pbwire.AppendTag(wire, 30, pbwire.WireVarint)
	wire = pbwire.AppendVarint(wire, 7)
	var kept [][]byte
	got := decodeTag2(t, wire, func(_ any, raw []byte) { kept = append(kept, append([]byte(nil), raw...)) })
	if len(got.Nums) != 3 {
		t.Fatalf("known fields lost: %+v", got)
	}
	if len(kept) != 1 {
		t.Fatalf("kept = %d raws, want 1", len(kept))
	}
	var want []byte
	want = pbwire.AppendTag(want, 30, pbwire.WireVarint)
	want = pbwire.AppendVarint(want, 7)
	if !bytes.Equal(kept[0], want) {
		t.Fatalf("kept = %x, want %x", kept[0], want)
	}
}

// TestFastTag2CollisionFallback pins first-byte collisions: fields 17
// (tag 136) and 33 (tag 264) share first byte 0x88, so both must decode
// through the generic path without mis-dispatch.
func TestFastTag2CollisionFallback(t *testing.T) {
	type cp struct {
		A []int32
		B []int32
	}
	var p cp
	mk := func(f *pbwire.Field) *pbwire.Field { f.Fast = true; return f }
	off := func(o uintptr) uint32 { return uint32(o) }
	tab := pbwire.NewTable("tag2coll", []*pbwire.Field{
		mk(&pbwire.Field{Num: 17, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Repeated: true, Off: off(unsafe.Offsetof(p.A)),
			GetRepI32: func(m any) []int32 { return m.(*cp).A },
			AppendI32: func(m any, v int32) { q := m.(*cp); q.A = append(q.A, v) }}),
		mk(&pbwire.Field{Num: 33, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Repeated: true, Off: off(unsafe.Offsetof(p.B)),
			GetRepI32: func(m any) []int32 { return m.(*cp).B },
			AppendI32: func(m any, v int32) { q := m.(*cp); q.B = append(q.B, v) }}),
	})
	var b []byte
	b = pbwire.AppendTag(b, 17, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 11)
	b = pbwire.AppendTag(b, 33, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 12)
	d := &pbwire.Decoder{Buf: b}
	a := &pbwire.Arena{}
	got := &cp{}
	if err := pbwire.DecodeTable(d, a, got, tab, 0); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(got.A) != 1 || got.A[0] != 11 || len(got.B) != 1 || got.B[0] != 12 {
		t.Fatalf("got %+v, want A=[11] B=[12]", got)
	}
}

// TestFastTag2Truncation pins bounds checks: a dangling first tag byte
// must fail, never panic.
func TestFastTag2Truncation(t *testing.T) {
	wire := tag2Wire(t)
	cut := append(append([]byte(nil), wire...), 0x8A)
	d := &pbwire.Decoder{Buf: cut}
	a := &pbwire.Arena{}
	if err := pbwire.DecodeTable(d, a, &tag2Probe{}, tag2Table(), 0); err == nil {
		t.Fatal("dangling 2-byte tag prefix decoded without error, want ErrWire")
	}
}
