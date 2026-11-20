// Package v2 is the Erlang-NIF-shaped Go probe: the schema plan is resolved
// once, the language-specific walk lives in C, and Marshal/Unmarshal each
// cross the cgo boundary exactly once.
//
// The Go half owns the struct layout.  At New time it measures every field
// with unsafe.Offsetof, builds a layout table (one row per field), and hands
// it to the C codec, which copies it and caches field-number -> row
// pointers.  Marshal passes the struct address as an integer for the
// duration of one call; C reads the Go memory through the offsets.  That
// keeps the cgo pointer-passing rules happy (no Go pointer crosses as a
// pointer value and none is retained), while runtime.KeepAlive keeps the
// reached-from-struct graph alive for the call.
//
// Decode ownership: C writes scalars directly into a zeroed Go struct and
// leaves string payloads in the caller's wire bytes, nested sub-messages in
// a codec-owned C arena and repeated elements in per-field accumulators.
// Go copies every string into one Go arena and every repeated field into one
// Go backing array before Unmarshal returns.  Strings are interior pointers
// into that arena, which keeps it alive; the C arena and accumulators are
// reset at the start of the next decode.
package cshim

/*
#cgo CFLAGS: -I${SRCDIR} -O2 -std=c99
#include "cshim_codec.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime"
	"unsafe"

	shimlayout "pbwire/pbench/cshim/layout"
	"pbwire/tests/gen/matrix3"
)

// Layout kinds.  These mirror the CSHIMK_* enum in cshim_codec.h.
const (
	kindI32 uint32 = iota
	kindI64
	kindU32
	kindU64
	kindS32
	kindS64
	kindFix32
	kindFix64
	kindFloat
	kindDouble
	kindBool
	kindStr
	kindMsg
	kindOptI32
	kindOptStr
)

// Wire types.
const (
	wireVarint  uint32 = 0
	wireFixed64 uint32 = 1
	wireBytes   uint32 = 2
	wireFixed32 uint32 = 5
)

// Status codes.  These mirror the CSHIM_* enum in cshim_codec.h.
const (
	stOK    = 0
	stNoMem = 1
	stWire  = 2
	stDepth = 3
	stPlan  = 4
	stSpace = 5
)

// layout holds the Go struct offsets measured once at New time.  C receives
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

// Codec owns the C plan, the decode scratch arena and the per-field repeated
// accumulators, plus the last encode size used as the next call's buffer
// hint.  It is not safe for concurrent use: one codec is a serialized
// encoder/decoder like the Erlang NIF env.
type Codec struct {
	h    *C.cshim_codec
	l    layout
	hint int
}

// New resolves the layout table once and compiles the C plan.
func New() (*Codec, error) {
	l := measure()
	rows, subrows := l.plan()
	c, err := newCodecFromCRows(rows, subrows,
		C.size_t(unsafe.Sizeof(matrix3.Scalars3{})),
		C.size_t(unsafe.Sizeof(matrix3.Inner3{})))
	if err != nil {
		return nil, err
	}
	c.l = l
	return c, nil
}

func newCodecFromCRows(rows, subrows []C.cshim_row, baseSize, subSize C.size_t) (*Codec, error) {
	var h *C.cshim_codec
	st := C.cshim_codec_new(&h, &rows[0], C.size_t(len(rows)),
		&subrows[0], C.size_t(len(subrows)), baseSize, subSize)
	runtime.KeepAlive(rows)
	runtime.KeepAlive(subrows)
	if int(st) != stOK {
		return nil, statusError("new", st, 0)
	}
	c := &Codec{h: h}
	runtime.SetFinalizer(c, (*Codec).finalize)
	return c, nil
}

// translateRow converts one layout.Row into the C row the codec owns.
// Shapes the C codec does not implement yet (oneof members, foreign
// fields, groups, sub-table references) fail loudly instead of encoding
// wrong bytes. Repeated rows pass through: the C codec already frames
// packed numerics, strings and sub-messages element by element.
func translateRow(r shimlayout.Row, ptrSize, subSize uintptr) (C.cshim_row, error) {
	if r.Itab != 0 || r.Foreign || r.Group || r.Sub >= 0 {
		return C.cshim_row{}, fmt.Errorf("cshim: field %d needs a later task (oneof/foreign/group/sub)", r.Fnum)
	}
	var kind C.uint32_t
	switch r.Kind {
	case shimlayout.KindInt32:
		kind = C.CSHIMK_I32
	case shimlayout.KindInt64:
		kind = C.CSHIMK_I64
	case shimlayout.KindUint32:
		kind = C.CSHIMK_U32
	case shimlayout.KindUint64:
		kind = C.CSHIMK_U64
	case shimlayout.KindSint32:
		kind = C.CSHIMK_S32
	case shimlayout.KindSint64:
		kind = C.CSHIMK_S64
	case shimlayout.KindFixed32:
		kind = C.CSHIMK_FIX32
	case shimlayout.KindFixed64:
		kind = C.CSHIMK_FIX64
	case shimlayout.KindSfixed32:
		kind = C.CSHIMK_FIX32
	case shimlayout.KindSfixed64:
		kind = C.CSHIMK_FIX64
	case shimlayout.KindFloat:
		kind = C.CSHIMK_FLOAT
	case shimlayout.KindDouble:
		kind = C.CSHIMK_DOUBLE
	case shimlayout.KindBool:
		kind = C.CSHIMK_BOOL
	case shimlayout.KindString:
		kind = C.CSHIMK_STR
	case shimlayout.KindBytes:
		kind = C.CSHIMK_BYTES
	case shimlayout.KindEnum:
		kind = C.CSHIMK_I32
	case shimlayout.KindMessage:
		kind = C.CSHIMK_MSG
	default:
		return C.cshim_row{}, fmt.Errorf("cshim: bad kind %d on field %d", r.Kind, r.Fnum)
	}
	var elem uint32
	switch r.Kind {
	case shimlayout.KindInt32, shimlayout.KindUint32, shimlayout.KindSint32,
		shimlayout.KindFixed32, shimlayout.KindSfixed32, shimlayout.KindFloat,
		shimlayout.KindEnum:
		elem = 4
	case shimlayout.KindInt64, shimlayout.KindUint64, shimlayout.KindSint64,
		shimlayout.KindFixed64, shimlayout.KindSfixed64, shimlayout.KindDouble:
		elem = 8
	case shimlayout.KindBool:
		elem = 1
	case shimlayout.KindString, shimlayout.KindBytes:
		elem = 2 * uint32(ptrSize)
	case shimlayout.KindMessage:
		elem = uint32(subSize)
	}
	var count uint32
	if (r.Kind == shimlayout.KindString || r.Kind == shimlayout.KindBytes) && !r.Explicit {
		count = r.Offset + uint32(ptrSize)
	}
	var has uint32
	if r.Explicit {
		has = r.Has
	}
	var explicit, required uint32
	if r.Explicit {
		explicit = 1
	}
	if r.Required {
		required = 1
	}
	return C.cshim_row{
		fnum:     C.uint32_t(r.Fnum),
		kind:     kind,
		repeated: C.uint32_t(boolBit(r.Repeated)),
		packed:   C.uint32_t(boolBit(r.Packed)),
		wire:     C.uint32_t(r.Wire),
		offset:   C.uint32_t(r.Offset),
		has:      C.uint32_t(has),
		count:    C.uint32_t(count),
		elem:     C.uint32_t(elem),
		explicit: C.uint32_t(explicit),
		required: C.uint32_t(required),
	}, nil
}

func boolBit(v bool) uint32 {
	if v {
		return 1
	}
	return 0
}

// newCodecFromLayout builds a codec from layout rows, the same shape the
// generated cshim files will feed the harness with. No per-message Go
// code lives here: translation is field-by-field and generic.
func newCodecFromLayout(rows, subrows []shimlayout.Row, baseSize, subSize uintptr) (*Codec, error) {
	if len(rows) == 0 || len(subrows) == 0 || baseSize == 0 || subSize == 0 {
		return nil, errors.New("v2: empty layout")
	}
	ptrSize := unsafe.Sizeof(uintptr(0))
	crows := make([]C.cshim_row, len(rows))
	for i, r := range rows {
		cr, err := translateRow(r, ptrSize, subSize)
		if err != nil {
			return nil, err
		}
		crows[i] = cr
	}
	csubs := make([]C.cshim_row, len(subrows))
	for i, r := range subrows {
		cr, err := translateRow(r, ptrSize, subSize)
		if err != nil {
			return nil, err
		}
		csubs[i] = cr
	}
	return newCodecFromCRows(crows, csubs, C.size_t(baseSize), C.size_t(subSize))
}

func (l *layout) plan() ([]C.cshim_row, []C.cshim_row) {
	ptr := l.ptrSize
	strElem := uintptr(unsafe.Sizeof(""))
	msgPtr := uintptr(unsafe.Sizeof((*matrix3.Inner3)(nil)))

	row := func(fnum, kind, repeated, packed, wire uint32, off, has, count,
		elem uintptr) C.cshim_row {
		return C.cshim_row{
			fnum:     C.uint32_t(fnum),
			kind:     C.uint32_t(kind),
			repeated: C.uint32_t(repeated),
			packed:   C.uint32_t(packed),
			wire:     C.uint32_t(wire),
			offset:   C.uint32_t(off),
			has:      C.uint32_t(has),
			count:    C.uint32_t(count),
			elem:     C.uint32_t(elem),
		}
	}
	scalar := func(fnum, kind, wire uint32, off, elem uintptr) C.cshim_row {
		return row(fnum, kind, 0, 0, wire, off, 0, 0, elem)
	}

	main := []C.cshim_row{
		scalar(1, kindI32, wireVarint, l.i32, 4),
		scalar(2, kindI64, wireVarint, l.i64, 8),
		scalar(3, kindU32, wireVarint, l.u32, 4),
		scalar(4, kindU64, wireVarint, l.u64, 8),
		scalar(5, kindS32, wireVarint, l.s32, 4),
		scalar(6, kindS64, wireVarint, l.s64, 8),
		scalar(7, kindFix32, wireFixed32, l.fx32, 4),
		scalar(8, kindFix64, wireFixed64, l.fx64, 8),
		scalar(9, kindFix32, wireFixed32, l.sfx32, 4),
		scalar(10, kindFix64, wireFixed64, l.sfx64, 8),
		scalar(11, kindFloat, wireFixed32, l.f32, 4),
		scalar(12, kindDouble, wireFixed64, l.f64, 8),
		scalar(13, kindBool, wireVarint, l.flag, 1),
		row(14, kindStr, 0, 0, wireBytes, l.str, 0, l.str+ptr, strElem),
		scalar(15, kindI32, wireVarint, l.color, 4),
		row(16, kindMsg, 0, 0, wireBytes, l.inner, 0, 0, l.innerSize),
		row(17, kindI32, 1, 1, wireVarint, l.nums, 0, l.nums+ptr, 4),
		row(18, kindStr, 1, 0, wireBytes, l.tags, 0, l.tags+ptr, strElem),
		row(19, kindMsg, 1, 0, wireBytes, l.inners, 0, l.inners+ptr, msgPtr),
		row(20, kindOptI32, 0, 0, wireVarint, l.optI32, l.optI32, 0, 4),
		row(21, kindOptStr, 0, 0, wireBytes, l.optStr, l.optStr, 0,
			strElem),
		row(22, kindBool, 1, 1, wireVarint, l.flags, 0, l.flags+ptr, 1),
	}

	sub := []C.cshim_row{
		row(1, kindStr, 0, 0, wireBytes, l.innerName, 0,
			l.innerName+ptr, strElem),
		scalar(2, kindI32, wireVarint, l.innerValue, 4),
	}
	return main, sub
}

// Name reports the probe variant name.
func (c *Codec) Name() string { return "v2" }

// Close releases the C plan and decode arena.  A finished Codec must not be
// used again.
func (c *Codec) Close() {
	if c == nil || c.h == nil {
		return
	}
	C.cshim_codec_free(c.h)
	c.h = nil
	runtime.SetFinalizer(c, nil)
}

func (c *Codec) finalize() {
	c.Close()
}

// Marshal encodes m into fresh Go bytes.  C writes into a Go-allocated
// buffer, so the fast path is one allocation and no post-encode copy.  If
// the previous size hint is too small, the call reports that and the loop
// doubles the hint and retries; the returned bytes are caller-owned.
func (c *Codec) Marshal(m *matrix3.Scalars3) ([]byte, error) {
	if c == nil || c.h == nil {
		return nil, errors.New("v2: closed codec")
	}
	if m == nil {
		return nil, errors.New("v2: nil message")
	}
	out, err := c.encodeRaw(unsafe.Pointer(m))
	runtime.KeepAlive(m)
	return out, err
}

// encodeRaw runs one grow-and-retry encode loop over raw struct memory.
// The caller owns the layout the codec was built with; no fixup runs.
func (c *Codec) encodeRaw(base unsafe.Pointer) ([]byte, error) {
	hint := c.hint
	if hint < 64 {
		hint = 64
	}
	for {
		out := make([]byte, hint)
		var dst *C.uint8_t
		if hint != 0 {
			dst = (*C.uint8_t)(unsafe.Pointer(&out[0]))
		}
		res := C.cshim_encode(c.h, C.uintptr_t(uintptr(base)), dst,
			C.size_t(hint))
		switch int(res.status) {
		case stOK:
			n := int(res.len)
			c.hint = n
			runtime.KeepAlive(c)
			return out[:n], nil
		case stSpace:
			if hint > int(^uint(0)>>1)/2 {
				return nil, statusError("marshal", C.int(stNoMem), 0)
			}
			hint *= 2
			continue
		default:
			return nil, statusError("marshal", res.status, 0)
		}
	}
}

// Unmarshal decodes wire into a fully materialized Go struct.  One cgo call
// fills the zeroed struct with scalars plus raw wire-backed string headers,
// C-arena sub-messages and repeated accumulators; fixup copies strings into
// one Go arena and repeated fields into Go backing arrays, after which the
// C scratch can be reused.
func (c *Codec) Unmarshal(wire []byte) (*matrix3.Scalars3, error) {
	if c == nil || c.h == nil {
		return nil, errors.New("v2: closed codec")
	}
	m := &matrix3.Scalars3{}
	strBytes, err := c.decodeRaw(unsafe.Pointer(m), wire)
	if err != nil {
		return nil, err
	}
	c.fixup(m, strBytes)
	runtime.KeepAlive(c)
	return m, nil
}

// decodeRaw runs one decode into raw struct memory and reports the total
// string payload size for the caller's fixup. No fixup runs here.
func (c *Codec) decodeRaw(base unsafe.Pointer, wire []byte) (int, error) {
	var wp *C.uint8_t
	if len(wire) != 0 {
		wp = (*C.uint8_t)(unsafe.Pointer(&wire[0]))
	}
	res := C.cshim_decode(c.h, C.uintptr_t(uintptr(base)), wp,
		C.size_t(len(wire)))
	runtime.KeepAlive(wire)
	if int(res.status) != stOK {
		return 0, statusError("unmarshal", res.status, res.fail_off)
	}
	runtime.KeepAlive(c)
	return int(res.str_bytes), nil
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

// decodeArena packs every decoded string into one Go []byte.  The returned
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

func statusError(op string, st C.int, off C.size_t) error {
	var msg string

	switch int(st) {
	case stNoMem:
		msg = "out of memory"
	case stWire:
		msg = "malformed wire bytes"
	case stDepth:
		msg = "nesting limit exceeded"
	case stPlan:
		msg = "layout plan rejected"
	case stSpace:
		msg = "output buffer too small"
	default:
		msg = fmt.Sprintf("status %d", int(st))
	}
	if off != 0 {
		return fmt.Errorf("v2: %s: %s at byte %d", op, msg, uint64(off))
	}
	return fmt.Errorf("v2: %s: %s", op, msg)
}
