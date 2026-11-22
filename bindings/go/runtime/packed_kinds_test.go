package runtime_test

// Characterization net for packed runs (1.3x bar): every packable kind
// must round-trip through packed encoding, including multi-byte values,
// zigzag extremes and a truncated tail. The monomorphized packed loops
// mirror decodeRepeated's packed branches exactly; this pins them.
import (
	"math"
	"testing"
	"unsafe"

	pbwire "pbwire/runtime"
)

type packedProbe struct {
	A []int32
	B []int64
	C []uint32
	D []uint64
	E []int32
	F []int64
	G []uint32
	H []uint64
	I []int32
	J []int64
	K []float32
	L []float64
	M []bool
	N []int32
}

func packedProbeTable() *pbwire.Table {
	p := packedProbe{}
	off := func(o uintptr) uint32 { return uint32(o) }
	rep := func(f *pbwire.Field) *pbwire.Field { f.Repeated = true; f.Packed = true; f.Fast = true; return f }
	return pbwire.NewTable("packedprobe", []*pbwire.Field{
		rep(&pbwire.Field{Num: 1, Wire: pbwire.WireVarint, Kind: pbwire.KindInt32, Off: off(unsafe.Offsetof(p.A)), GetRepI32: func(m any) []int32 { return m.(*packedProbe).A }, AppendI32: func(m any, v int32) { q := m.(*packedProbe); q.A = append(q.A, v) }}),
		rep(&pbwire.Field{Num: 2, Wire: pbwire.WireVarint, Kind: pbwire.KindInt64, Off: off(unsafe.Offsetof(p.B)), GetRepI64: func(m any) []int64 { return m.(*packedProbe).B }, AppendI64: func(m any, v int64) { q := m.(*packedProbe); q.B = append(q.B, v) }}),
		rep(&pbwire.Field{Num: 3, Wire: pbwire.WireVarint, Kind: pbwire.KindUint32, Off: off(unsafe.Offsetof(p.C)), GetRepU32: func(m any) []uint32 { return m.(*packedProbe).C }, AppendU32: func(m any, v uint32) { q := m.(*packedProbe); q.C = append(q.C, v) }}),
		rep(&pbwire.Field{Num: 4, Wire: pbwire.WireVarint, Kind: pbwire.KindUint64, Off: off(unsafe.Offsetof(p.D)), GetRepU64: func(m any) []uint64 { return m.(*packedProbe).D }, AppendU64: func(m any, v uint64) { q := m.(*packedProbe); q.D = append(q.D, v) }}),
		rep(&pbwire.Field{Num: 5, Wire: pbwire.WireVarint, Kind: pbwire.KindSint32, Off: off(unsafe.Offsetof(p.E)), GetRepS32: func(m any) []int32 { return m.(*packedProbe).E }, AppendS32: func(m any, v int32) { q := m.(*packedProbe); q.E = append(q.E, v) }}),
		rep(&pbwire.Field{Num: 6, Wire: pbwire.WireVarint, Kind: pbwire.KindSint64, Off: off(unsafe.Offsetof(p.F)), GetRepS64: func(m any) []int64 { return m.(*packedProbe).F }, AppendS64: func(m any, v int64) { q := m.(*packedProbe); q.F = append(q.F, v) }}),
		rep(&pbwire.Field{Num: 7, Wire: pbwire.WireFixed32, Kind: pbwire.KindFixed32, Off: off(unsafe.Offsetof(p.G)), GetRepFx32: func(m any) []uint32 { return m.(*packedProbe).G }, AppendFx32: func(m any, v uint32) { q := m.(*packedProbe); q.G = append(q.G, v) }}),
		rep(&pbwire.Field{Num: 8, Wire: pbwire.WireFixed64, Kind: pbwire.KindFixed64, Off: off(unsafe.Offsetof(p.H)), GetRepFx64: func(m any) []uint64 { return m.(*packedProbe).H }, AppendFx64: func(m any, v uint64) { q := m.(*packedProbe); q.H = append(q.H, v) }}),
		rep(&pbwire.Field{Num: 9, Wire: pbwire.WireFixed32, Kind: pbwire.KindSfixed32, Off: off(unsafe.Offsetof(p.I)), GetRepSfx32: func(m any) []int32 { return m.(*packedProbe).I }, AppendSfx32: func(m any, v int32) { q := m.(*packedProbe); q.I = append(q.I, v) }}),
		rep(&pbwire.Field{Num: 10, Wire: pbwire.WireFixed64, Kind: pbwire.KindSfixed64, Off: off(unsafe.Offsetof(p.J)), GetRepSfx64: func(m any) []int64 { return m.(*packedProbe).J }, AppendSfx64: func(m any, v int64) { q := m.(*packedProbe); q.J = append(q.J, v) }}),
		rep(&pbwire.Field{Num: 11, Wire: pbwire.WireFixed32, Kind: pbwire.KindFloat, Off: off(unsafe.Offsetof(p.K)), GetRepF32: func(m any) []float32 { return m.(*packedProbe).K }, AppendF32: func(m any, v float32) { q := m.(*packedProbe); q.K = append(q.K, v) }}),
		rep(&pbwire.Field{Num: 12, Wire: pbwire.WireFixed64, Kind: pbwire.KindDouble, Off: off(unsafe.Offsetof(p.L)), GetRepF64: func(m any) []float64 { return m.(*packedProbe).L }, AppendF64: func(m any, v float64) { q := m.(*packedProbe); q.L = append(q.L, v) }}),
		rep(&pbwire.Field{Num: 13, Wire: pbwire.WireVarint, Kind: pbwire.KindBool, Off: off(unsafe.Offsetof(p.M)), GetRepBool: func(m any) []bool { return m.(*packedProbe).M }, AppendBool: func(m any, v bool) { q := m.(*packedProbe); q.M = append(q.M, v) }}),
		rep(&pbwire.Field{Num: 14, Wire: pbwire.WireVarint, Kind: pbwire.KindEnum, Off: off(unsafe.Offsetof(p.N)), GetRepEnum: func(m any) []int32 { return m.(*packedProbe).N }, AppendEnum: func(m any, v int32) { q := m.(*packedProbe); q.N = append(q.N, v) }}),
	})
}

func packedProbeWire(t *testing.T) []byte {
	t.Helper()
	neg1 := int64(-1)
	sfx32min := int32(math.MinInt32)
	sfx64min := int64(math.MinInt64)
	var b []byte
	run := func(num uint32, wt pbwire.WireType, vs []uint64, fixed int) {
		// Packed runs always carry a length-delimited tag; wt selects
		// only the payload width below.
		b = pbwire.AppendTag(b, num, pbwire.WireLen)
		_ = wt
		var payload []byte
		for _, v := range vs {
			switch fixed {
			case 4:
				payload = pbwire.AppendFixed32(payload, uint32(v))
			case 8:
				payload = pbwire.AppendFixed64(payload, v)
			default:
				payload = pbwire.AppendVarint(payload, v)
			}
		}
		b = pbwire.AppendVarint(b, uint64(len(payload)))
		b = append(b, payload...)
	}
	run(1, pbwire.WireVarint, []uint64{1, 300, uint64(neg1)}, 0)
	run(2, pbwire.WireVarint, []uint64{uint64(math.MaxInt64), 7}, 0)
	run(3, pbwire.WireVarint, []uint64{uint64(math.MaxUint32), 8}, 0)
	run(4, pbwire.WireVarint, []uint64{math.MaxUint64, 9}, 0)
	run(5, pbwire.WireVarint, []uint64{pbwire.ZigZagEncode32(math.MinInt32), pbwire.ZigZagEncode32(300)}, 0)
	run(6, pbwire.WireVarint, []uint64{pbwire.ZigZagEncode64(math.MinInt64), pbwire.ZigZagEncode64(-300)}, 0)
	run(7, pbwire.WireFixed32, []uint64{0xdeadbeef, 1}, 4)
	run(8, pbwire.WireFixed64, []uint64{72623859790382856, 2}, 8)
	run(9, pbwire.WireFixed32, []uint64{uint64(uint32(sfx32min)), 3}, 4)
	run(10, pbwire.WireFixed64, []uint64{uint64(sfx64min), 4}, 8)
	run(11, pbwire.WireFixed32, []uint64{uint64(math.Float32bits(float32(math.Inf(1)))), uint64(math.Float32bits(5))}, 4)
	run(12, pbwire.WireFixed64, []uint64{math.Float64bits(-123456.789), math.Float64bits(6)}, 8)
	run(13, pbwire.WireVarint, []uint64{1, 0, 1}, 0)
	run(14, pbwire.WireVarint, []uint64{2, 300}, 0)
	return b
}

// TestPackedAllKinds pins every packable kind through packed runs.
func TestPackedAllKinds(t *testing.T) {
	tab := packedProbeTable()
	d := &pbwire.Decoder{Buf: packedProbeWire(t)}
	a := &pbwire.Arena{}
	got := &packedProbe{}
	if err := pbwire.DecodeTable(d, a, got, tab, 0); err != nil {
		t.Fatal(err)
	}
	if len(got.A) != 3 || got.A[0] != 1 || got.A[1] != 300 || got.A[2] != -1 {
		t.Fatalf("I32 packed: %+v", got.A)
	}
	if len(got.B) != 2 || got.B[0] != math.MaxInt64 || got.B[1] != 7 {
		t.Fatalf("I64 packed: %+v", got.B)
	}
	if len(got.C) != 2 || got.C[0] != math.MaxUint32 || got.C[1] != 8 {
		t.Fatalf("U32 packed: %+v", got.C)
	}
	if len(got.D) != 2 || got.D[0] != math.MaxUint64 || got.D[1] != 9 {
		t.Fatalf("U64 packed: %+v", got.D)
	}
	if len(got.E) != 2 || got.E[0] != math.MinInt32 || got.E[1] != 300 {
		t.Fatalf("S32 packed: %+v", got.E)
	}
	if len(got.F) != 2 || got.F[0] != math.MinInt64 || got.F[1] != -300 {
		t.Fatalf("S64 packed: %+v", got.F)
	}
	if len(got.G) != 2 || got.G[0] != 0xdeadbeef || got.G[1] != 1 {
		t.Fatalf("Fx32 packed: %+v", got.G)
	}
	if len(got.H) != 2 || got.H[0] != 72623859790382856 || got.H[1] != 2 {
		t.Fatalf("Fx64 packed: %+v", got.H)
	}
	if len(got.I) != 2 || got.I[0] != math.MinInt32 || got.I[1] != 3 {
		t.Fatalf("Sfx32 packed: %+v", got.I)
	}
	if len(got.J) != 2 || got.J[0] != math.MinInt64 || got.J[1] != 4 {
		t.Fatalf("Sfx64 packed: %+v", got.J)
	}
	if len(got.K) != 2 || got.K[0] != float32(math.Inf(1)) || got.K[1] != 5.0 {
		t.Fatalf("F32 packed: %+v", got.K)
	}
	if len(got.L) != 2 || got.L[0] != -123456.789 || got.L[1] != 6.0 {
		t.Fatalf("F64 packed: %+v", got.L)
	}
	if len(got.M) != 3 || !got.M[0] || got.M[1] || !got.M[2] {
		t.Fatalf("Bool packed: %+v", got.M)
	}
	if len(got.N) != 2 || got.N[0] != 2 || got.N[1] != 300 {
		t.Fatalf("Enum packed: %+v", got.N)
	}
	// Truncated tail: the last packed run loses its final byte.
	full := packedProbeWire(t)
	if err := pbwire.DecodeTable(&pbwire.Decoder{Buf: full[:len(full)-1]}, &pbwire.Arena{}, &packedProbe{}, tab, 0); err == nil {
		t.Fatal("truncated packed tail decoded without error, want ErrWire")
	}
}
