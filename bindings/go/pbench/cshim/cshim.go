// Package cshim is the Erlang-style candidate arm over its vendored engine
// (engine/): the schema plan is resolved once, the walk lives in C, and
// Marshal/Unmarshal each cross the cgo boundary exactly once. The matrix3
// plan and fixup below are the hand-written stand-in for what
// protoc-gen-pbwire emits per message; the generic engine lives in engine/.
package cshim

import (
	"errors"
	"runtime"
	"unsafe"

	pbwiruntime "pbwire/pbench/cshim/engine"
	shimlayout "pbwire/pbench/cshim/layout"
	"pbwire/tests/gen/matrix3"
)

// Codec owns the runtime plan plus the matrix3 struct layout measured once
// at New time. It is not safe for concurrent use.
type Codec struct {
	rt *pbwiruntime.Codec
	l  layout
}

// layout holds the Go struct offsets measured once at New time. C receives
// them as uint32 row fields and never computes an offset itself.
type layout struct {
	i32, i64, u32, u64    uintptr
	s32, s64              uintptr
	fx32, fx64            uintptr
	sfx32, sfx64          uintptr
	f32, f64              uintptr
	flag, str, color      uintptr
	inner                 uintptr
	nums, tags, inners    uintptr
	optI32, optStr, flags uintptr

	innerName, innerValue uintptr
	ptrSize               uintptr
	innerSize             uintptr
}

func measure() layout {
	var m matrix3.Scalars3
	var in matrix3.Inner3

	return layout{
		i32:    unsafe.Offsetof(m.I32),
		i64:    unsafe.Offsetof(m.I64),
		u32:    unsafe.Offsetof(m.U32),
		u64:    unsafe.Offsetof(m.U64),
		s32:    unsafe.Offsetof(m.S32),
		s64:    unsafe.Offsetof(m.S64),
		fx32:   unsafe.Offsetof(m.Fx32),
		fx64:   unsafe.Offsetof(m.Fx64),
		sfx32:  unsafe.Offsetof(m.Sfx32),
		sfx64:  unsafe.Offsetof(m.Sfx64),
		f32:    unsafe.Offsetof(m.F32),
		f64:    unsafe.Offsetof(m.F64),
		flag:   unsafe.Offsetof(m.Flag),
		str:    unsafe.Offsetof(m.Str),
		color:  unsafe.Offsetof(m.Color),
		inner:  unsafe.Offsetof(m.Inner),
		nums:   unsafe.Offsetof(m.Nums),
		tags:   unsafe.Offsetof(m.Tags),
		inners: unsafe.Offsetof(m.Inners),
		optI32: unsafe.Offsetof(m.OptI32),
		optStr: unsafe.Offsetof(m.OptStr),
		flags:  unsafe.Offsetof(m.Flags),

		innerName:  unsafe.Offsetof(in.Name),
		innerValue: unsafe.Offsetof(in.Value),
		ptrSize:    unsafe.Sizeof(uintptr(0)),
		innerSize:  unsafe.Sizeof(in),
	}
}

// New resolves the layout table once and compiles the C plan.
func New() (*Codec, error) {
	l := measure()
	rt, err := pbwiruntime.NewCodecFromLayout(l.plan(), "structtest.Scalars3")
	if err != nil {
		return nil, err
	}
	return &Codec{rt: rt, l: l}, nil
}

// plan is the hand-written stand-in for generated layout tables: one row
// per field, offsets measured once at init. Optional fields ride the
// generalized explicit shape (pointer at Has); enums ride int32.
func (l *layout) plan() []shimlayout.Msg {
	scalar := func(fnum uint32, kind shimlayout.Kind, wire shimlayout.Wire, off uintptr) shimlayout.Row {
		return shimlayout.Row{Fnum: fnum, Kind: kind, Wire: wire, Offset: uint32(off), Sub: -1}
	}
	rep := func(fnum uint32, kind shimlayout.Kind, wire shimlayout.Wire, off uintptr, packed bool, sub int32) shimlayout.Row {
		return shimlayout.Row{Fnum: fnum, Kind: kind, Repeated: true, Packed: packed, Wire: wire, Offset: uint32(off), Sub: sub}
	}
	return []shimlayout.Msg{
		{Full: "structtest.Scalars3", Size: unsafe.Sizeof(matrix3.Scalars3{}), Rows: []shimlayout.Row{
			scalar(1, shimlayout.KindInt32, shimlayout.WireVarint, l.i32),
			scalar(2, shimlayout.KindInt64, shimlayout.WireVarint, l.i64),
			scalar(3, shimlayout.KindUint32, shimlayout.WireVarint, l.u32),
			scalar(4, shimlayout.KindUint64, shimlayout.WireVarint, l.u64),
			scalar(5, shimlayout.KindSint32, shimlayout.WireVarint, l.s32),
			scalar(6, shimlayout.KindSint64, shimlayout.WireVarint, l.s64),
			scalar(7, shimlayout.KindFixed32, shimlayout.WireFixed32, l.fx32),
			scalar(8, shimlayout.KindFixed64, shimlayout.WireFixed64, l.fx64),
			scalar(9, shimlayout.KindSfixed32, shimlayout.WireFixed32, l.sfx32),
			scalar(10, shimlayout.KindSfixed64, shimlayout.WireFixed64, l.sfx64),
			scalar(11, shimlayout.KindFloat, shimlayout.WireFixed32, l.f32),
			scalar(12, shimlayout.KindDouble, shimlayout.WireFixed64, l.f64),
			scalar(13, shimlayout.KindBool, shimlayout.WireVarint, l.flag),
			scalar(14, shimlayout.KindString, shimlayout.WireLen, l.str),
			scalar(15, shimlayout.KindInt32, shimlayout.WireVarint, l.color),
			{Fnum: 16, Kind: shimlayout.KindMessage, Wire: shimlayout.WireLen, Offset: uint32(l.inner), Sub: 1},
			rep(17, shimlayout.KindInt32, shimlayout.WireVarint, l.nums, true, -1),
			rep(18, shimlayout.KindString, shimlayout.WireLen, l.tags, false, -1),
			rep(19, shimlayout.KindMessage, shimlayout.WireLen, l.inners, false, 1),
			{Fnum: 20, Kind: shimlayout.KindInt32, Wire: shimlayout.WireVarint, Offset: uint32(l.optI32), Has: uint32(l.optI32), Explicit: true, Sub: -1},
			{Fnum: 21, Kind: shimlayout.KindString, Wire: shimlayout.WireLen, Offset: uint32(l.optStr), Has: uint32(l.optStr), Explicit: true, Sub: -1},
			rep(22, shimlayout.KindBool, shimlayout.WireVarint, l.flags, true, -1),
		}},
		{Full: "structtest.Inner3", Size: unsafe.Sizeof(matrix3.Inner3{}), Rows: []shimlayout.Row{
			scalar(1, shimlayout.KindString, shimlayout.WireLen, l.innerName),
			scalar(2, shimlayout.KindInt32, shimlayout.WireVarint, l.innerValue),
		}},
	}
}

// Name reports the probe variant name.
func (c *Codec) Name() string { return "v2" }

// Close releases the C plan and decode arena. A finished Codec must not be
// used again.
func (c *Codec) Close() {
	if c == nil {
		return
	}
	c.rt.Close()
}

// Marshal encodes m into fresh Go bytes. C writes into a Go-allocated
// buffer, so the fast path is one allocation and no post-encode copy.
func (c *Codec) Marshal(m *matrix3.Scalars3) ([]byte, error) {
	if c == nil || c.rt == nil {
		return nil, errors.New("cshim: closed codec")
	}
	if m == nil {
		return nil, errors.New("cshim: nil message")
	}
	out, err := c.rt.EncodeRaw(unsafe.Pointer(m))
	runtime.KeepAlive(m)
	if err != nil {
		return nil, err
	}
	return out, nil
}

// Unmarshal decodes wire into a fully materialized Go struct. One cgo call
// fills the zeroed struct with scalars plus raw wire-backed string headers,
// C-arena sub-messages and repeated accumulators; fixup copies strings into
// one Go arena and repeated fields into Go backing arrays, after which the
// C scratch can be reused.
func (c *Codec) Unmarshal(wire []byte) (*matrix3.Scalars3, error) {
	if c == nil || c.rt == nil {
		return nil, errors.New("cshim: closed codec")
	}
	m := &matrix3.Scalars3{}
	strBytes, err := c.rt.DecodeRaw(unsafe.Pointer(m), wire)
	if err != nil {
		return nil, err
	}
	c.fixup(m, strBytes)
	runtime.KeepAlive(c)
	return m, nil
}

// rawHeader and rawSlice are the Go header layouts C writes during decode.
// They are read-only views of codec scratch/wire; fixup must replace them
// before Unmarshal returns.
type rawHeader struct {
	data unsafe.Pointer
	len  uintptr
}

type rawSlice struct {
	data unsafe.Pointer
	len  uintptr
	cap  uintptr
}

// decodeArena packs every decoded string into one Go []byte. The returned
// string headers point inside the arena; that interior pointer keeps the
// whole backing array alive after Unmarshal returns.
type decodeArena struct {
	buf []byte
	off int
}

func (a *decodeArena) str(h *rawHeader) string {
	if h == nil || h.len == 0 {
		return ""
	}
	n := int(h.len)
	start := a.off
	a.off += n
	copy(a.buf[start:start+n], unsafe.Slice((*byte)(h.data), n))
	return unsafe.String(&a.buf[start], n)
}

// decodeExtras holds the root nested message and proto3 optional payloads in
// one allocation instead of one heap object per pointer field.
type decodeExtras struct {
	inner  matrix3.Inner3
	optI32 int32
	optStr string
}

func (c *Codec) fixup(m *matrix3.Scalars3, strBytes int) {
	l := &c.l
	base := unsafe.Pointer(m)

	var arena decodeArena
	if strBytes != 0 {
		arena.buf = make([]byte, strBytes)
	}

	m.Str = arena.str((*rawHeader)(unsafe.Add(base, l.str)))

	ip := *(*unsafe.Pointer)(unsafe.Add(base, l.inner))
	p32 := *(*unsafe.Pointer)(unsafe.Add(base, l.optI32))
	pstr := *(*unsafe.Pointer)(unsafe.Add(base, l.optStr))
	var ex *decodeExtras
	if ip != nil || p32 != nil || pstr != nil {
		ex = &decodeExtras{}
	}
	if ip != nil {
		ex.inner.Name = arena.str((*rawHeader)(unsafe.Add(ip, l.innerName)))
		ex.inner.Value = *(*int32)(unsafe.Add(ip, l.innerValue))
		m.Inner = &ex.inner
	} else {
		m.Inner = nil
	}
	if p32 != nil {
		ex.optI32 = *(*int32)(p32)
		m.OptI32 = &ex.optI32
	} else {
		m.OptI32 = nil
	}
	if pstr != nil {
		ex.optStr = arena.str((*rawHeader)(pstr))
		m.OptStr = &ex.optStr
	} else {
		m.OptStr = nil
	}

	rh := (*rawSlice)(unsafe.Add(base, l.nums))
	if rh.len == 0 {
		m.Nums = nil
	} else {
		dst := make([]int32, int(rh.len))
		copy(dst, unsafe.Slice((*int32)(rh.data), rh.len))
		m.Nums = dst
	}

	rh = (*rawSlice)(unsafe.Add(base, l.tags))
	if rh.len == 0 {
		m.Tags = nil
	} else {
		src := unsafe.Slice((*rawHeader)(rh.data), rh.len)
		dst := make([]string, int(rh.len))
		for i := range dst {
			dst[i] = arena.str(&src[i])
		}
		m.Tags = dst
	}

	rh = (*rawSlice)(unsafe.Add(base, l.inners))
	if rh.len == 0 {
		m.Inners = nil
	} else {
		n := int(rh.len)
		src := unsafe.Slice((*unsafe.Pointer)(rh.data), n)
		vals := make([]matrix3.Inner3, n)
		dst := make([]*matrix3.Inner3, n)
		for i := range dst {
			p := src[i]
			if p == nil {
				continue
			}
			vals[i].Name = arena.str((*rawHeader)(unsafe.Add(p, l.innerName)))
			vals[i].Value = *(*int32)(unsafe.Add(p, l.innerValue))
			dst[i] = &vals[i]
		}
		m.Inners = dst
	}

	rh = (*rawSlice)(unsafe.Add(base, l.flags))
	if rh.len == 0 {
		m.Flags = nil
	} else {
		src := unsafe.Slice((*uint8)(rh.data), rh.len)
		dst := make([]bool, int(rh.len))
		for i := range dst {
			dst[i] = src[i] != 0
		}
		m.Flags = dst
	}
}
