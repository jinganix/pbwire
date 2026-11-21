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
//   - Decode stores plain integers, floats and bools only. Storing a
//     pointer (string header, slice header, optional allocation,
//     submessage) through unsafe.Pointer would skip the write barrier, so
//     those rows stay on closures, which perform the typed stores with
//     their write barriers intact.
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
	if f.Oneof || f.Foreign != nil || f.Group || f.Kind == KindMessage {
		return false, nil
	}
	if f.Repeated {
		return encodeFastRepeated(e, base, f)
	}
	addr := unsafe.Add(base, int(f.Off))
	switch f.Kind {
	case KindInt32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int32)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(int64(v)))
	case KindInt64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int64)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(v))
	case KindUint32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*uint32)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
			return true, nil
		}
		v := *(*uint32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(v))
	case KindUint64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*uint64)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(v)
			return true, nil
		}
		v := *(*uint64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(v)
	case KindSint32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int32)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode32(v))
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(ZigZagEncode32(v))
	case KindSint64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int64)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode64(v))
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(ZigZagEncode64(v))
	case KindFixed32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*uint32)(p)
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(v)
			return true, nil
		}
		v := *(*uint32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(v)
	case KindFixed64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*uint64)(p)
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(v)
			return true, nil
		}
		v := *(*uint64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(v)
	case KindSfixed32:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int32)(p)
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(uint32(v))
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(uint32(v))
	case KindSfixed64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int64)(p)
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(uint64(v))
			return true, nil
		}
		v := *(*int64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(uint64(v))
	case KindFloat:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*float32)(p)
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(math.Float32bits(v))
			return true, nil
		}
		v := *(*float32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(math.Float32bits(v))
	case KindDouble:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*float64)(p)
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(math.Float64bits(v))
			return true, nil
		}
		v := *(*float64)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(math.Float64bits(v))
	case KindBool:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*bool)(p)
			e.Tag(f.Num, WireVarint)
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
			return true, nil
		}
		v := *(*bool)(addr)
		if !v && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		if v {
			e.Varint(1)
		} else {
			e.Varint(0)
		}
	case KindEnum:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*int32)(p)
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
			return true, nil
		}
		v := *(*int32)(addr)
		if v == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(int64(v)))
	case KindString:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*string)(p)
			e.Tag(f.Num, WireLen)
			e.Text(v)
			return true, nil
		}
		v := *(*string)(addr)
		if v == "" && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireLen)
		e.Text(v)
	case KindBytes:
		if f.Explicit {
			p := *(*unsafe.Pointer)(unsafe.Add(base, int(f.Has)))
			if p == nil {
				return true, nil
			}
			v := *(*[]byte)(p)
			e.Tag(f.Num, WireLen)
			e.Data(v)
			return true, nil
		}
		v := *(*[]byte)(addr)
		if len(v) == 0 && !f.Required {
			return true, nil
		}
		e.Tag(f.Num, WireLen)
		e.Data(v)
	default:
		return false, nil
	}
	return true, nil
}

// encodeFastRepeated writes one repeated scalar row from a slice header
// read straight from the struct. Named element types (enums) share the
// int32 layout and are read the same way; the bytes are what matters.
func encodeFastRepeated(e *Encoder, base unsafe.Pointer, f *Field) (bool, error) {
	addr := unsafe.Add(base, int(f.Off))
	switch f.Kind {
	case KindInt32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(int64(v)))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		}
	case KindInt64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		}
	case KindUint32:
		vs := *(*[]uint32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		}
	case KindUint64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]uint64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(v)
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(v)
		}
	case KindSint32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(ZigZagEncode32(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode32(v))
		}
	case KindSint64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(ZigZagEncode64(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode64(v))
		}
	case KindFixed32:
		vs := *(*[]uint32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(v)
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(v)
		}
	case KindFixed64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]uint64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(v)
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(v)
		}
	case KindSfixed32:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(uint32(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(uint32(v))
		}
	case KindSfixed64:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]int64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(uint64(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(uint64(v))
		}
	case KindFloat:
		vs := *(*[]float32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(math.Float32bits(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(math.Float32bits(v))
		}
	case KindDouble:
		if !aligned8(uintptr(addr)) {
			return false, nil
		}
		vs := *(*[]float64)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(math.Float64bits(v))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(math.Float64bits(v))
		}
	case KindBool:
		vs := *(*[]bool)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				if v {
					e.Varint(1)
				} else {
					e.Varint(0)
				}
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
		}
	case KindString:
		vs := *(*[]string)(addr)
		for _, v := range vs {
			e.Tag(f.Num, WireLen)
			e.Text(v)
		}
	case KindBytes:
		vs := *(*[][]byte)(addr)
		for _, v := range vs {
			e.Tag(f.Num, WireLen)
			e.Data(v)
		}
	case KindEnum:
		vs := *(*[]int32)(addr)
		if len(vs) == 0 {
			return true, nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(int64(v)))
			}
			e.End(mark)
			return true, nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		}
	default:
		return false, nil
	}
	return true, nil
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
