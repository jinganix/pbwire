package runtime_test

// Characterization net for the decode loop fast path (1.1x bar, serial
// small/medium decode): plain Fast scalar singulars must decode to
// identical values through the fast path and the generic decodeField path,
// and a known field carrying the wrong wire type must stay dropped as
// unknown data. Every closure below panics: neither path may call one for
// these rows, so any fallback to per-field getters fails loudly.
import (
	"math"
	"testing"
	"unsafe"

	pbwire "pbwire/runtime"
)

type fastScalarProbe struct {
	I32   int32
	I64   int64
	U32   uint32
	U64   uint64
	S32   int32
	S64   int64
	Fx32  uint32
	Fx64  uint64
	Sfx32 int32
	Sfx64 int64
	F32   float32
	F64   float64
	Flag  bool
	En    int32
}

func fastPanic[T any]() func(any) T {
	return func(any) T { panic("fastScalarProbe: fell back to closure on a fast row") }
}

func fastScalarTable() *pbwire.Table {
	mk := func(f *pbwire.Field) *pbwire.Field { f.Fast = true; return f }
	off := func(o uintptr) uint32 { return uint32(o) }
	p := fastScalarProbe{}
	return pbwire.NewTable("fastscalar", []*pbwire.Field{
		mk(&pbwire.Field{Num: 1, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Off: off(unsafe.Offsetof(p.I32)), GetI32: fastPanic[int32](), SetI32: func(any, int32) { panic("x") }}),
		mk(&pbwire.Field{Num: 2, Wire: pbwire.WireVarint, Kind: pbwire.KindInt64, Off: off(unsafe.Offsetof(p.I64)), GetI64: fastPanic[int64](), SetI64: func(any, int64) { panic("x") }}),
		mk(&pbwire.Field{Num: 3, Wire: pbwire.WireVarint, Kind: pbwire.KindUint32, Off: off(unsafe.Offsetof(p.U32)), GetU32: fastPanic[uint32](), SetU32: func(any, uint32) { panic("x") }}),
		mk(&pbwire.Field{Num: 4, Wire: pbwire.WireVarint, Kind: pbwire.KindUint64, Off: off(unsafe.Offsetof(p.U64)), GetU64: fastPanic[uint64](), SetU64: func(any, uint64) { panic("x") }}),
		mk(&pbwire.Field{Num: 5, Wire: pbwire.WireVarint, Kind: pbwire.KindSint32, Off: off(unsafe.Offsetof(p.S32)), GetS32: fastPanic[int32](), SetS32: func(any, int32) { panic("x") }}),
		mk(&pbwire.Field{Num: 6, Wire: pbwire.WireVarint, Kind: pbwire.KindSint64, Off: off(unsafe.Offsetof(p.S64)), GetS64: fastPanic[int64](), SetS64: func(any, int64) { panic("x") }}),
		mk(&pbwire.Field{Num: 7, Wire: pbwire.WireFixed32, Kind: pbwire.KindFixed32, Off: off(unsafe.Offsetof(p.Fx32)), GetFx32: fastPanic[uint32](), SetFx32: func(any, uint32) { panic("x") }}),
		mk(&pbwire.Field{Num: 8, Wire: pbwire.WireFixed64, Kind: pbwire.KindFixed64, Off: off(unsafe.Offsetof(p.Fx64)), GetFx64: fastPanic[uint64](), SetFx64: func(any, uint64) { panic("x") }}),
		mk(&pbwire.Field{Num: 9, Wire: pbwire.WireFixed32, Kind: pbwire.KindSfixed32, Off: off(unsafe.Offsetof(p.Sfx32)), GetSfx32: fastPanic[int32](), SetSfx32: func(any, int32) { panic("x") }}),
		mk(&pbwire.Field{Num: 10, Wire: pbwire.WireFixed64, Kind: pbwire.KindSfixed64, Off: off(unsafe.Offsetof(p.Sfx64)), GetSfx64: fastPanic[int64](), SetSfx64: func(any, int64) { panic("x") }}),
		mk(&pbwire.Field{Num: 11, Wire: pbwire.WireFixed32, Kind: pbwire.KindFloat, Off: off(unsafe.Offsetof(p.F32)), GetF32: fastPanic[float32](), SetF32: func(any, float32) { panic("x") }}),
		mk(&pbwire.Field{Num: 12, Wire: pbwire.WireFixed64, Kind: pbwire.KindDouble, Off: off(unsafe.Offsetof(p.F64)), GetF64: fastPanic[float64](), SetF64: func(any, float64) { panic("x") }}),
		mk(&pbwire.Field{Num: 13, Wire: pbwire.WireVarint, Kind: pbwire.KindBool, Off: off(unsafe.Offsetof(p.Flag)), GetBool: fastPanic[bool](), SetBool: func(any, bool) { panic("x") }}),
		mk(&pbwire.Field{Num: 14, Wire: pbwire.WireVarint, Kind: pbwire.KindEnum, Off: off(unsafe.Offsetof(p.En)), GetEnum: fastPanic[int32](), SetEnum: func(any, int32) { panic("x") }}),
	})
}

func fastScalarWire() []byte {
	neg1 := int64(-1)
	sfx32min := int32(math.MinInt32)
	sfx64min := int64(math.MinInt64)
	var b []byte
	b = pbwire.AppendTag(b, 1, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, uint64(neg1)) // 10-byte varint
	b = pbwire.AppendTag(b, 2, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, uint64(math.MaxInt64))
	b = pbwire.AppendTag(b, 3, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, uint64(math.MaxUint32))
	b = pbwire.AppendTag(b, 4, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, uint64(math.MaxUint64))
	b = pbwire.AppendTag(b, 5, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, pbwire.ZigZagEncode32(math.MinInt32))
	b = pbwire.AppendTag(b, 6, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, pbwire.ZigZagEncode64(math.MinInt64))
	b = pbwire.AppendTag(b, 7, pbwire.WireFixed32)
	b = pbwire.AppendFixed32(b, 0xdeadbeef)
	b = pbwire.AppendTag(b, 8, pbwire.WireFixed64)
	b = pbwire.AppendFixed64(b, 72623859790382856)
	b = pbwire.AppendTag(b, 9, pbwire.WireFixed32)
	b = pbwire.AppendFixed32(b, uint32(sfx32min))
	b = pbwire.AppendTag(b, 10, pbwire.WireFixed64)
	b = pbwire.AppendFixed64(b, uint64(sfx64min))
	b = pbwire.AppendTag(b, 11, pbwire.WireFixed32)
	b = pbwire.AppendFixed32(b, math.Float32bits(float32(math.Inf(1))))
	b = pbwire.AppendTag(b, 12, pbwire.WireFixed64)
	b = pbwire.AppendFixed64(b, math.Float64bits(-123456.789))
	b = pbwire.AppendTag(b, 13, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 1)
	b = pbwire.AppendTag(b, 14, pbwire.WireVarint)
	b = pbwire.AppendVarint(b, 2)
	// Field 1 carrying fixed32 instead of varint: wrong wire type, so it
	// decodes as unknown data (dropped, no hook) and field 1 stays zero.
	b = pbwire.AppendTag(b, 1, pbwire.WireFixed32)
	b = pbwire.AppendFixed32(b, 0x01020304)
	return b
}

// TestDecodeFastScalarParity pins every plain scalar kind through Fast
// rows: multi-byte varints, zigzag extremes, fixed widths, inf floats,
// bool, enum, plus a wrong-wire-type occurrence dropped as unknown.
func TestDecodeFastScalarParity(t *testing.T) {
	tab := fastScalarTable()
	d := &pbwire.Decoder{Buf: fastScalarWire()}
	a := &pbwire.Arena{}
	got := &fastScalarProbe{}
	if err := pbwire.DecodeTable(d, a, got, tab, 0); err != nil {
		t.Fatal(err)
	}
	want := fastScalarProbe{
		I32: -1, I64: math.MaxInt64, U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64,
		Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64,
		F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, En: 2,
	}
	// The trailing fixed32 occurrence of field 1 is dropped as unknown
	// (wrong wire type), so the earlier varint -1 stands.
	if *got != want {
		t.Fatalf("fast scalar decode mismatch:\n got %+v\nwant %+v", *got, want)
	}
}
