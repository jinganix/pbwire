// Offset fast path: struct memory read directly inside the shared
// table engine.
//
// A row with Fast set carries layout facts (Off/Has/Count/Elem) over the
// same Go struct the closures address. The engine then reads the struct
// memory directly instead of calling a getter per field. Everything
// stays in Go, so there is no cgo crossing.
//
// Safety rules, stated once:
//
//   - Encode reads struct memory only. Loads need no write barriers, so
//     the whole singular and repeated scalar/string/bytes surface is fast.
//   - Decode stores plain integers, floats and bools directly, and also
//     strings, byte slices and optional slots through TYPED unsafe stores
//     (*(*string)(addr) = v and friends). A typed store of a
//     pointer-containing value still emits the write barrier; only hiding
//     a pointer as a uintptr would skip it, which this file never does.
//     Submessage creation still needs the row closure (only generated code
//     knows the concrete type to allocate), so those rows stay on closures.
//   - 64-bit kinds check 8-byte alignment at run time and fall back to
//     closures otherwise, so 32-bit targets cannot fault.
//   - 64-bit kinds check 8-byte alignment at run time and fall back to
//     closures otherwise, so 32-bit targets cannot fault.
//   - Rows the path cannot express (oneof, Foreign, Group, KindMessage)
//     return handled=false and run the existing closures. Fallback is per
//     row, so partial coverage still pays and output stays identical.
package runtime

import (
	"math"
	"unsafe"
)

// fastBase returns the struct address behind msg for tables with offset
// rows, or nil when the fast path does not apply. msg always holds a
// pointer here (generated models pass *T, the engine passes *DMsg for
// tree rows, whose tables never set hasFast); the interface data word of
// a pointer-typed value is that pointer, so no reflection runs.
func fastBase(t *Table, msg any) unsafe.Pointer {
	if t == nil || !t.hasFast || msg == nil {
		return nil
	}
	return (*[2]unsafe.Pointer)(unsafe.Pointer(&msg))[1]
}

// aligned8 reports whether addr is safe for a 64-bit access.
func aligned8(addr uintptr) bool { return addr%8 == 0 }

// encodeFast writes one row straight from struct memory. It returns
// handled=false for rows that stay on closures.
func encodeFast(e *Encoder, base unsafe.Pointer, f *Field, depth int) (bool, error) {
	// buf is the whole row on the stack: every append below stays off the
	// heap write path and lands with the single writeback before return.
	// A barrier per append is what stops parallel whole-message encodes
	// from scaling; see setCapHint for the shared-write half of that.
	buf := e.Buf
	if f.Oneof || f.Foreign != nil || f.Group || f.Entry || f.Kind == KindMessage {
		e.Buf = buf
		return false, nil
	}
	if f.Repeated {
		e.Buf = buf
		return encodeFastRepeated(e, base, f)
	}
	addr := unsafe.Add(base, int(f.Off))
	switch f.Kind {
	case KindInt32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int32)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(int64(v)))
			e.Buf = buf
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, uint64(int64(v)))
	case KindInt64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int64)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, uint64(v))
	case KindUint32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*uint32)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*uint32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, uint64(v))
	case KindUint64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*uint64)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, v)
			e.Buf = buf
			return true, nil
		}
		v := *(*uint64)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, v)
	case KindSint32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int32)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, ZigZagEncode32(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, ZigZagEncode32(v))
	case KindSint64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int64)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, ZigZagEncode64(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, ZigZagEncode64(v))
	case KindFixed32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*uint32)(p)
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, v)
			e.Buf = buf
			return true, nil
		}
		v := *(*uint32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed32)
		buf = AppendFixed32(buf, v)
	case KindFixed64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*uint64)(p)
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, v)
			e.Buf = buf
			return true, nil
		}
		v := *(*uint64)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed64)
		buf = AppendFixed64(buf, v)
	case KindSfixed32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int32)(p)
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, uint32(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed32)
		buf = AppendFixed32(buf, uint32(v))
	case KindSfixed64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int64)(p)
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, uint64(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed64)
		buf = AppendFixed64(buf, uint64(v))
	case KindFloat:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*float32)(p)
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, math.Float32bits(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*float32)(addr)
		// Negative zero is a value, not the default: compare bits so
		// -0.0 still emits, like the closure path and the reference
		// serializer (a plain == 0 would drop it).
		if math.Float32bits(v) == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed32)
		buf = AppendFixed32(buf, math.Float32bits(v))
	case KindDouble:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*float64)(p)
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, math.Float64bits(v))
			e.Buf = buf
			return true, nil
		}
		v := *(*float64)(addr)
		// Same bit comparison as float32 above: -0.0 emits.
		if math.Float64bits(v) == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireFixed64)
		buf = AppendFixed64(buf, math.Float64bits(v))
	case KindBool:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*bool)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			if v {
				buf = AppendVarint(buf, 1)
			} else {
				buf = AppendVarint(buf, 0)
			}
			e.Buf = buf
			return true, nil
		}
		v := *(*bool)(addr)
		if !v && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		if v {
			buf = AppendVarint(buf, 1)
		} else {
			buf = AppendVarint(buf, 0)
		}
	case KindEnum:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*int32)(p)
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(int64(v)))
			e.Buf = buf
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireVarint)
		buf = AppendVarint(buf, uint64(int64(v)))
	case KindString:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*string)(p)
			buf = AppendTag(buf, f.Num, WireLen)
			buf = AppendVarint(buf, uint64(len(v)))
			buf = append(buf, v...)
			e.Buf = buf
			return true, nil
		}
		v := *(*string)(addr)
		if v == "" && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireLen)
		buf = AppendVarint(buf, uint64(len(v)))
		buf = append(buf, v...)
	case KindBytes:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				e.Buf = buf
				return true, nil
			}
			v := *(*[]byte)(p)
			buf = AppendTag(buf, f.Num, WireLen)
			buf = AppendLen(buf, v)
			e.Buf = buf
			return true, nil
		}
		v := *(*[]byte)(addr)
		if len(v) == 0 && !f.Required {
			e.Buf = buf
			return true, nil
		}
		buf = AppendTag(buf, f.Num, WireLen)
		buf = AppendLen(buf, v)
	default:
		e.Buf = buf
		return false, nil
	}
	e.Buf = buf
	return true, nil
}

// encodeFastRepeated writes one repeated scalar row from a slice header
// read straight from the struct. Named element types (enums) share the
// int32 layout and are read the same way; the bytes are what matters.
func encodeFastRepeated(e *Encoder, base unsafe.Pointer, f *Field) (bool, error) {
	// Same stack-buffer discipline as encodeFast above.
	buf := e.Buf
	addr := unsafe.Add(base, int(f.Off))
	switch f.Kind {
	case KindInt32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, uint64(int64(v)))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(int64(v)))
		}
	case KindInt64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, uint64(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(v))
		}
	case KindUint32:
		vs := *(*[]uint32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, uint64(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(v))
		}
	case KindUint64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]uint64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, v)
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, v)
		}
	case KindSint32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, ZigZagEncode32(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, ZigZagEncode32(v))
		}
	case KindSint64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, ZigZagEncode64(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, ZigZagEncode64(v))
		}
	case KindFixed32:
		vs := *(*[]uint32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed32(buf, v)
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, v)
		}
	case KindFixed64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]uint64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed64(buf, v)
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, v)
		}
	case KindSfixed32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed32(buf, uint32(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, uint32(v))
		}
	case KindSfixed64:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed64(buf, uint64(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, uint64(v))
		}
	case KindFloat:
		vs := *(*[]float32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed32(buf, math.Float32bits(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed32)
			buf = AppendFixed32(buf, math.Float32bits(v))
		}
	case KindDouble:
		if !aligned8(uintptr(addr)) {
			e.Buf = buf
			return false, nil
		}
		vs := *(*[]float64)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendFixed64(buf, math.Float64bits(v))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireFixed64)
			buf = AppendFixed64(buf, math.Float64bits(v))
		}
	case KindBool:
		vs := *(*[]bool)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				if v {
					buf = AppendVarint(buf, 1)
				} else {
					buf = AppendVarint(buf, 0)
				}
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			if v {
				buf = AppendVarint(buf, 1)
			} else {
				buf = AppendVarint(buf, 0)
			}
		}
	case KindString:
		vs := *(*[]string)(addr)
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireLen)
			buf = AppendVarint(buf, uint64(len(v)))
			buf = append(buf, v...)
		}
	case KindBytes:
		vs := *(*[][]byte)(addr)
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireLen)
			buf = AppendLen(buf, v)
		}
	case KindEnum:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			e.Buf = buf
			return true, nil
		}
		if f.Packed {
			buf, mark := beginLen(buf, f.Num)
			for _, v := range vs {
				buf = AppendVarint(buf, uint64(int64(v)))
			}
			buf = endLen(buf, mark)
			e.Buf = buf
			return true, nil
		}
		for _, v := range vs {
			buf = AppendTag(buf, f.Num, WireVarint)
			buf = AppendVarint(buf, uint64(int64(v)))
		}
	default:
		e.Buf = buf
		return false, nil
	}
	e.Buf = buf
	return true, nil
}

// storeFastOpt stores one decoded optional payload: it allocates the
// pointer slot's element generically (the kind fixes T, so no generated
// code runs) and publishes it with a typed store, which keeps the write
// barrier. The allocation itself still escapes to the heap, exactly like
// the row closure it replaces; what disappears is the interface call and
// the type assertion per occurrence.
func storeFastOpt[T any](base unsafe.Pointer, f *Field, v T) bool {
	if base == nil || !f.Fast {
		return false
	}
	p := new(T)
	*p = v
	*(*unsafe.Pointer)(unsafe.Add(base, uintptr(f.Has))) = unsafe.Pointer(p)
	return true
}

// storeFastStr stores one decoded string payload with a typed store (write
// barrier intact). Explicit rows allocate the *string slot instead.
func storeFastStr(base unsafe.Pointer, f *Field, v string) bool {
	if base == nil || !f.Fast {
		return false
	}
	if f.Explicit {
		return storeFastOpt(base, f, v)
	}
	*(*string)(unsafe.Add(base, uintptr(f.Off))) = v
	return true
}

// storeFastBytes stores one decoded bytes payload with a typed store
// (write barrier intact). Explicit rows allocate the *[]byte slot instead.
func storeFastBytes(base unsafe.Pointer, f *Field, v []byte) bool {
	if base == nil || !f.Fast {
		return false
	}
	if f.Explicit {
		return storeFastOpt(base, f, v)
	}
	*(*[]byte)(unsafe.Add(base, uintptr(f.Off))) = v
	return true
}

// appendFast appends one decoded repeated element straight to the slice
// header in the struct: the load/append/store-back is one generic body
// for every scalar kind (named element types share the layout), closed
// over by the engine's packed and unpacked loops. The store-back is typed,
// so the write barrier is intact; growth follows append semantics exactly
// like the row closure it replaces.
func appendFast[T any](base unsafe.Pointer, f *Field, v T) bool {
	if base == nil || !f.Fast {
		return false
	}
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]T)(addr)
	s = append(s, v)
	*(*[]T)(addr) = s
	return true
}

// packedNeed bounds how many packed elements remain: exact for fixed
// widths and bools, clamped head start for varints (see the
// appendPacked* drains below).
func packedNeed(kind Kind, remain int) int {
	switch kind {
	case KindFixed32, KindSfixed32, KindFloat:
		return remain / 4
	case KindFixed64, KindSfixed64, KindDouble:
		return remain / 8
	case KindBool:
		return remain
	default:
		// Varint kinds: one wire byte minimum per element, so remain
		// bounds the count; the head start is clamped to 128 elements
		// (at most 1KB for 8-byte slots) so hostile 10-byte varints
		// cannot amplify unboundedly. Runs up to 128 elements (every
		// standard tier) still size exactly in one allocation.
		if remain > 128 {
			return 128
		}
		return remain
	}
}

// presizeFast gives an unpacked repeated slice a head start on its first
// element, estimated from the input still unread: occurrences arrive one
// per field appearance, so only the first can size ahead. need is already
// clamped by the caller; later occurrences keep geometric growth.
func presizeFast[T any](base unsafe.Pointer, f *Field, need int) {
	if base == nil || !f.Fast || need <= 0 {
		return
	}
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]T)(addr)
	if len(s) != 0 || cap(s) != 0 {
		return
	}
	*(*[]T)(addr) = make([]T, 0, need)
}

// unpackedNeed estimates how many unpacked occurrences remain from the
// unread input bytes: one tag byte plus length plus payload each, six for
// a small string average. It is only a size hint (shorter occurrences
// under-size and grow as before), clamped so hostile input cannot inflate
// beyond 64 slots per field.
func unpackedNeed(remain int) int {
	need := remain / 6
	if need < 8 {
		// Singletons and tiny runs grow exactly as before: a head start
		// below 8 slots saves at most 3 growths while wasting capacity
		// on every 1-element field (medium tier B/op regressed). Bulk
		// runs keep the presize.
		return 0
	}
	if need > 64 {
		return 64
	}
	return need
}

// fastWantWire is the wire type a plain Fast scalar row accepts, indexed
// by Kind. String/Bytes/Message and any out-of-range Kind map to 0xFF, so
// the loop gate rejects them without a switch; the zero value (WireVarint)
// never matches a real gate because uncovered Kinds carry the sentinel.
var fastWantWire = [256]WireType{
	KindInt32:    WireVarint,
	KindInt64:    WireVarint,
	KindUint32:   WireVarint,
	KindUint64:   WireVarint,
	KindSint32:   WireVarint,
	KindSint64:   WireVarint,
	KindFixed32:  WireFixed32,
	KindFixed64:  WireFixed64,
	KindSfixed32: WireFixed32,
	KindSfixed64: WireFixed64,
	KindFloat:    WireFixed32,
	KindDouble:   WireFixed64,
	KindBool:     WireVarint,
	KindEnum:     WireVarint,
	KindString:   0xFF,
	KindBytes:    0xFF,
	KindMessage:  0xFF,
}

// fastWantAlign64 reports the 64-bit kinds whose struct slot needs an
// 8-byte alignment check before a direct store (see aligned8): 32-bit
// targets cannot fault, the row just stays on the generic path.
var fastWantAlign64 = [256]bool{
	KindInt64: true, KindUint64: true, KindSint64: true,
	KindSfixed64: true, KindFixed64: true, KindDouble: true,
}

// Monomorphized packed-run drains: one function per packable kind, built
// for the loop in decodeRepeated the way decodeFastScalar is built for
// the singular loop. Each parses its width (1-byte varints inline, fixed
// widths by call — both inline into the caller), transforms exactly like
// decodeRepeated's packed branch, and appends straight to the struct
// slice. A shared generic engine cannot do this: the element type differs
// per kind, so a kind switch inside it would not compile. The presize
// keeps the single-growth behavior the alloc bound pins.
func appendPackedI32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int32)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, int32(v))
	}
	*(*[]int32)(addr) = s
	return nil
}
func appendPackedI64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int64)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, int64(v))
	}
	*(*[]int64)(addr) = s
	return nil
}
func appendPackedU32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[uint32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]uint32)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, uint32(v))
	}
	*(*[]uint32)(addr) = s
	return nil
}
func appendPackedU64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[uint64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]uint64)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, v)
	}
	*(*[]uint64)(addr) = s
	return nil
}
func appendPackedS32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int32)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, ZigZagDecode32(v))
	}
	*(*[]int32)(addr) = s
	return nil
}
func appendPackedS64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int64)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, ZigZagDecode64(v))
	}
	*(*[]int64)(addr) = s
	return nil
}
func appendPackedFx32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[uint32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]uint32)(addr)
	for !sub.Done() {
		v, err := sub.Fixed32()
		if err != nil {
			return err
		}
		s = append(s, v)
	}
	*(*[]uint32)(addr) = s
	return nil
}
func appendPackedFx64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[uint64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]uint64)(addr)
	for !sub.Done() {
		v, err := sub.Fixed64()
		if err != nil {
			return err
		}
		s = append(s, v)
	}
	*(*[]uint64)(addr) = s
	return nil
}
func appendPackedSfx32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int32)(addr)
	for !sub.Done() {
		v, err := sub.Fixed32()
		if err != nil {
			return err
		}
		s = append(s, int32(v))
	}
	*(*[]int32)(addr) = s
	return nil
}
func appendPackedSfx64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int64)(addr)
	for !sub.Done() {
		v, err := sub.Fixed64()
		if err != nil {
			return err
		}
		s = append(s, int64(v))
	}
	*(*[]int64)(addr) = s
	return nil
}
func appendPackedF32(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[float32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]float32)(addr)
	for !sub.Done() {
		v, err := sub.Fixed32()
		if err != nil {
			return err
		}
		s = append(s, math.Float32frombits(v))
	}
	*(*[]float32)(addr) = s
	return nil
}
func appendPackedF64(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[float64](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]float64)(addr)
	for !sub.Done() {
		v, err := sub.Fixed64()
		if err != nil {
			return err
		}
		s = append(s, math.Float64frombits(v))
	}
	*(*[]float64)(addr) = s
	return nil
}
func appendPackedBool(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[bool](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]bool)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, v != 0)
	}
	*(*[]bool)(addr) = s
	return nil
}
func appendPackedEnum(base unsafe.Pointer, f *Field, sub *Decoder) error {
	presizeFast[int32](base, f, packedNeed(f.Kind, len(sub.Buf)-sub.Off))
	addr := unsafe.Add(base, uintptr(f.Off))
	s := *(*[]int32)(addr)
	for !sub.Done() {
		v, ok := sub.varintByte()
		if !ok {
			var err error
			v, err = sub.Varint()
			if err != nil {
				return err
			}
		}
		s = append(s, int32(v))
	}
	*(*[]int32)(addr) = s
	return nil
}

// length placeholder, returning the buffer and the mark for endLen.
func beginLen(buf []byte, num uint32) ([]byte, int) {
	buf = AppendTag(buf, num, WireLen)
	mark := len(buf)
	return append(buf, 0), mark
}

// endLen is (*Encoder).End on a local buffer: it patches the length opened
// by beginLen, shifting the payload right when the length itself needs more
// than one byte. Identical bytes to Encoder.End, or the gate fails.
func endLen(buf []byte, mark int) []byte {
	n := len(buf) - mark - 1
	if n < 0x80 {
		buf[mark] = byte(n)
		return buf
	}
	var tmp [10]byte
	v := uint64(n)
	i := 0
	for v >= 0x80 {
		tmp[i] = byte(v) | 0x80
		v >>= 7
		i++
	}
	tmp[i] = byte(v)
	i++
	buf = append(buf, tmp[1:i]...)
	copy(buf[mark+i:], buf[mark+1:])
	copy(buf[mark:], tmp[:i])
	return buf
}

// storeFast stores one parsed scalar payload into the struct field. u
// carries the final value's bits (floats as Float32/64bits, bools as 0/1).
// Only plain integer-width stores happen here — no pointers cross unsafe,
// so no write barrier is skipped. Everything else returns false and the
// caller runs the row's setter closure. base is the fastBase of the
// message being filled (nil when the table has no offset rows), computed
// once per message by the caller instead of once per field here.
func storeFast(base unsafe.Pointer, f *Field, u uint64) bool {
	if base == nil || !f.Fast || f.Oneof || f.Foreign != nil || f.Group ||
		f.Explicit || f.Repeated {
		return false
	}
	addr := unsafe.Add(base, int(f.Off))
	switch f.Kind {
	case KindInt32, KindSint32, KindSfixed32, KindEnum:
		*(*int32)(addr) = int32(u)
	case KindInt64, KindSint64, KindSfixed64:
		if !aligned8(uintptr(addr)) {
			return false
		}
		*(*int64)(addr) = int64(u)
	case KindUint32, KindFixed32:
		*(*uint32)(addr) = uint32(u)
	case KindUint64, KindFixed64:
		if !aligned8(uintptr(addr)) {
			return false
		}
		*(*uint64)(addr) = u
	case KindFloat:
		*(*float32)(addr) = math.Float32frombits(uint32(u))
	case KindDouble:
		if !aligned8(uintptr(addr)) {
			return false
		}
		*(*float64)(addr) = math.Float64frombits(u)
	case KindBool:
		*(*bool)(addr) = u != 0
	default:
		return false
	}
	return true
}
