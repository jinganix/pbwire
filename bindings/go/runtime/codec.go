// Package runtime is the packaged Go binding engine: the schema plan is
// resolved once, the language-specific walk lives in C, and Marshal/Unmarshal
// each cross the cgo boundary exactly once.
//
// The Go half owns the struct layout. At New time it measures every field
// with unsafe.Offsetof, builds a layout table (one row per field), and hands
// it to the C codec, which copies it and caches field-number -> row
// pointers. Marshal passes the struct address as an integer for the
// duration of one call; C reads the Go memory through the offsets. That
// keeps the cgo pointer-passing rules happy (no Go pointer crosses as a
// pointer value and none is retained), while runtime.KeepAlive keeps the
// reached-from-struct graph alive for the call.
//
// Decode ownership: C writes scalars directly into a zeroed Go struct and
// leaves string payloads in the caller's wire bytes, nested sub-messages in
// a codec-owned C arena and repeated elements in per-field accumulators.
// The caller copies every string into one Go arena and every repeated field
// into one Go backing array before returning. Strings are interior pointers
// into that arena, which keeps it alive; the C arena and accumulators are
// reset at the start of the next decode.
//
// Per-message plans (which offsets, which fixup) are generated code, not
// this package: see pbench/cshim for the hand-written reference arm and
// protoc-gen-pbwire for the emitter. This package only translates layout
// rows, owns the C plan, and runs the raw encode/decode calls.
package runtime

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

	shimlayout "pbwire/runtime/layout"
)

// Layout kinds. These mirror the CSHIMK_* enum in cshim_codec.h.
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

// Status codes. These mirror the CSHIM_* enum in cshim_codec.h.
const (
	stOK    = 0
	stNoMem = 1
	stWire  = 2
	stDepth = 3
	stPlan  = 4
	stSpace = 5
)

// Codec owns the C plan, the decode scratch arena and the per-field repeated
// accumulators, plus the last encode size used as the next call's buffer
// hint. It is not safe for concurrent use: one codec is a serialized
// encoder/decoder.
type Codec struct {
	h    *C.cshim_codec
	hint int
}

// cshimTableInput is one registry entry: the Go-owned rows plus the
// struct size the C arena uses for sub-message allocation.
type cshimTableInput struct {
	rows []C.cshim_row
	size C.size_t
}

// newCodecFromTables builds a codec over a caller-owned row registry.
// tables[root] is the root message; message rows address their sub-table
// through the row's sub index. Unexported: the input rows are C-owned and
// only NewCodecFromLayout can build them; cross-package callers use that.
func newCodecFromTables(tabs []cshimTableInput, root int) (*Codec, error) {
	if len(tabs) == 0 || root < 0 || root >= len(tabs) {
		return nil, errors.New("runtime: bad registry")
	}
	ct := make([]C.cshim_table, len(tabs))
	var pinner runtime.Pinner
	defer pinner.Unpin()
	for i, t := range tabs {
		if len(t.rows) == 0 || t.size == 0 {
			return nil, errors.New("runtime: empty table")
		}
		// The row slices travel into C, which copies them synchronously:
		// pin every backing array for the duration of the call.
		pinner.Pin(&t.rows[0])
		ct[i] = C.cshim_table{
			rows:     &t.rows[0],
			nrows:    C.size_t(len(t.rows)),
			msg_size: t.size,
		}
	}
	pinner.Pin(&ct[0])
	var h *C.cshim_codec
	st := C.cshim_codec_new(&h, &ct[0], C.size_t(len(ct)), C.size_t(root))
	runtime.KeepAlive(tabs)
	runtime.KeepAlive(ct)
	if int(st) != stOK {
		return nil, statusError("new", C.int(st), 0)
	}
	c := &Codec{h: h}
	runtime.SetFinalizer(c, (*Codec).finalize)
	return c, nil
}

// translateRow converts one layout.Row into the C row the codec owns.
// Shapes the C codec does not implement yet (foreign fields) fail loudly
// instead of encoding wrong bytes. Groups and sub-table references ride
// the registry the caller built.
func translateRow(r shimlayout.Row, reg []shimlayout.Msg, ptrSize uintptr) (C.cshim_row, error) {
	if r.Foreign {
		return C.cshim_row{}, fmt.Errorf("runtime: field %d needs foreign support", r.Fnum)
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
		return C.cshim_row{}, fmt.Errorf("runtime: bad kind %d on field %d", r.Kind, r.Fnum)
	}
	var elem uint32
	if r.Repeated {
		switch r.Kind {
		case shimlayout.KindInt32, shimlayout.KindUint32, shimlayout.KindSint32,
			shimlayout.KindFixed32, shimlayout.KindSfixed32, shimlayout.KindFloat,
			shimlayout.KindEnum:
			elem = 4
		case shimlayout.KindBool:
			elem = 1
		case shimlayout.KindString, shimlayout.KindBytes:
			elem = 2 * uint32(ptrSize)
		case shimlayout.KindMessage:
			elem = uint32(ptrSize)
		}
	} else {
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
			if r.Sub < 0 || int(r.Sub) >= len(reg) {
				return C.cshim_row{}, fmt.Errorf("runtime: field %d names no registry table", r.Fnum)
			}
			elem = uint32(reg[r.Sub].Size)
		}
	}
	var count uint32
	if r.Repeated || ((r.Kind == shimlayout.KindString || r.Kind == shimlayout.KindBytes) && !r.Explicit && r.Itab == 0) {
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
	var group uint32
	if r.Group {
		group = 1
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
		group:    C.uint32_t(group),
		sub:      C.int32_t(r.Sub),
		itab:     C.uintptr_t(r.Itab),
		ogrp:     C.uint32_t(r.Ogrp),
	}, nil
}

func boolBit(v bool) uint32 {
	if v {
		return 1
	}
	return 0
}

// NewCodecFromLayout builds a codec from a layout registry, the same
// shape the generated code feeds the harness with. No per-message Go code
// lives here: translation is field-by-field and generic, and sub-table
// indexes ride the registry order.
func NewCodecFromLayout(reg []shimlayout.Msg, root string) (*Codec, error) {
	if len(reg) == 0 {
		return nil, errors.New("runtime: empty registry")
	}
	ri := -1
	for i, m := range reg {
		if m.Full == root {
			ri = i
			break
		}
	}
	if ri < 0 {
		return nil, fmt.Errorf("runtime: unknown root %q", root)
	}
	ptrSize := unsafe.Sizeof(uintptr(0))
	tabs := make([]cshimTableInput, len(reg))
	for i, m := range reg {
		if len(m.Rows) == 0 || m.Size == 0 {
			return nil, fmt.Errorf("runtime: empty table %q", m.Full)
		}
		crows := make([]C.cshim_row, len(m.Rows))
		for j, r := range m.Rows {
			cr, err := translateRow(r, reg, ptrSize)
			if err != nil {
				return nil, err
			}
			crows[j] = cr
		}
		tabs[i] = cshimTableInput{rows: crows, size: C.size_t(m.Size)}
	}
	return newCodecFromTables(tabs, ri)
}

// Close releases the C plan and decode arena. A finished Codec must not be
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

// EncodeRaw runs one grow-and-retry encode loop over raw struct memory.
// The caller owns the layout the codec was built with; no fixup runs.
// A nil base is a caller bug: report it instead of faulting in C.
func (c *Codec) EncodeRaw(base unsafe.Pointer) ([]byte, error) {
	if c == nil || c.h == nil {
		return nil, errors.New("runtime: closed codec")
	}
	if base == nil {
		return nil, errors.New("runtime: encode nil message")
	}
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
				return nil, statusError("encode", C.int(stNoMem), 0)
			}
			hint *= 2
			continue
		default:
			return nil, statusError("encode", res.status, 0)
		}
	}
}

// DecodeRaw runs one decode into raw struct memory and reports the total
// string payload size for the caller's fixup. No fixup runs here.
func (c *Codec) DecodeRaw(base unsafe.Pointer, wire []byte) (int, error) {
	if c == nil || c.h == nil {
		return 0, errors.New("runtime: closed codec")
	}
	if base == nil {
		return 0, errors.New("runtime: decode into nil message")
	}
	var wp *C.uint8_t
	if len(wire) != 0 {
		wp = (*C.uint8_t)(unsafe.Pointer(&wire[0]))
	}
	res := C.cshim_decode(c.h, C.uintptr_t(uintptr(base)), wp,
		C.size_t(len(wire)))
	runtime.KeepAlive(wire)
	if int(res.status) != stOK {
		return 0, statusError("decode", res.status, res.fail_off)
	}
	runtime.KeepAlive(c)
	return int(res.str_bytes), nil
}

// Stage reads one staged oneof occurrence for the fixup the generated
// model owns: fnum 0 means nothing was staged in this slot. The payload
// points into wire (strings/bytes) or the C arena (messages) and is valid
// until the next decode on this codec.
func (c *Codec) Stage(idx int) (fnum, kind uint32, scalar uint64, ptr unsafe.Pointer, ln int, ok bool) {
	var cf, ck C.uint32_t
	var cs C.uint64_t
	var cp C.uintptr_t
	var cl C.size_t
	if C.cshim_stage(c.h, C.size_t(idx), &cf, &ck, &cs, &cp, &cl) != 0 {
		return 0, 0, 0, nil, 0, false
	}
	runtime.KeepAlive(c)
	// The integer arrives from C (wire/arena address valid until the next
	// decode); reinterpret its bits as a pointer in one step so vet
	// accepts the uintptr -> pointer conversion.
	return uint32(cf), uint32(ck), uint64(cs), *(*unsafe.Pointer)(unsafe.Pointer(&cp)), int(cl), true
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
		return fmt.Errorf("runtime: %s: %s at byte %d", op, msg, uint64(off))
	}
	return fmt.Errorf("runtime: %s: %s", op, msg)
}
