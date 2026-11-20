// Table-driven codec: generated code emits only DATA (one Table per
// message, one Field row per proto field) and this file holds the ONE
// shared engine that interprets any table. No per-message straight-line
// encode/decode exists: EncodeTable walks rows calling getters and writes
// through the wire primitives, DecodeTable loops Decoder.Next, indexes the
// field by number, calls setters, and skips unknowns. Nested messages
// recurse through the row's Sub table (a lazy func so recursive messages
// cannot form a package-init cycle); cross-package messages go through the
// sibling's own Marshal/Unmarshal behind the Foreign hooks; proto2 groups
// frame with start/end tags and decode in a dedicated loop.
//
// Presence matches protoc-gen-go byte for byte: proto3-implicit scalars
// are skipped when zero (floats compare == 0, so -0.0 is omitted),
// proto3-optional and proto2-optional fields emit iff the pointer is
// non-nil, oneof members emit whatever is set (zero and empty included),
// required fields always emit. Repeated packable numerics pack on encode
// when the row says so and accept both packed runs and unpacked singles
// on decode, like protobuf-go. Unknown fields (wrong number, wrong wire
// type, extensions, groups-as-unknown) are skipped, never preserved: the
// struct has no storage for them.
package codec

import (
	"math"
	"sync/atomic"
)

// Kind identifies the protobuf scalar kind of one table row. The engine
// switches on it; generated code only names it.
type Kind uint8

const (
	KindInt32 Kind = iota + 1
	KindInt64
	KindUint32
	KindUint64
	KindSint32
	KindSint64
	KindFixed32
	KindFixed64
	KindSfixed32
	KindSfixed64
	KindFloat
	KindDouble
	KindBool
	KindString
	KindBytes
	KindEnum
	KindMessage
)

// ForeignOps carries a cross-package message across the package boundary:
// the sibling's own Marshal/Unmarshal do the wire work, the engine only
// frames the bytes. Generated code fills these closures; the engine never
// names a foreign type.
type ForeignOps struct {
	Encode func(msg any) ([]byte, error)
	Decode func(msg any, wire []byte) error
}

// Field is one row of a message table: field number, wire type, kind,
// presence rule, repetition flags, and trivial getter/setter function
// values on the Go struct. The closures only load/store struct fields
// (wrapping oneof members, dereferencing optional pointers, converting
// named enum types); all wire logic lives in EncodeTable/DecodeTable.
type Field struct {
	Name     string
	Num      uint32
	Wire     WireType
	Kind     Kind
	Explicit bool // optional pointer: presence is the pointer, not the value
	Required bool // proto2 required: always written, no presence guard
	Repeated bool
	Packed   bool // repeated packable numerics share one length run
	Group    bool // proto2 group: start/end framing (Kind is KindMessage)
	Oneof    bool // oneof member: presence via GetOneof, value always written
	// Fast and the columns below are the cshim-shaped layout facts.
	// Fast means the columns are valid and the engine may read/write the
	// struct field directly instead of calling the closures. Zero Fast
	// keeps the row on closures: offset 0 is a legal field offset (the
	// first struct field), so it cannot double as "unset". Only columns
	// the engine reads are kept.
	Fast    bool   // offset columns valid; direct struct access allowed
	Off     uint32 // byte offset of the value (pointer slot for explicit rows)
	Has     uint32 // byte offset of the presence pointer (explicit rows)
	Sub     func() *Table
	Foreign *ForeignOps

	GetI32    func(msg any) int32
	SetI32    func(msg any, v int32)
	GetRepI32 func(msg any) []int32
	AppendI32 func(msg any, v int32)

	GetRepI64 func(msg any) []int64
	AppendI64 func(msg any, v int64)

	GetRepU32 func(msg any) []uint32
	AppendU32 func(msg any, v uint32)

	GetRepU64 func(msg any) []uint64
	AppendU64 func(msg any, v uint64)

	GetRepS32 func(msg any) []int32
	AppendS32 func(msg any, v int32)

	GetRepS64 func(msg any) []int64
	AppendS64 func(msg any, v int64)

	GetRepFx32 func(msg any) []uint32
	AppendFx32 func(msg any, v uint32)

	GetRepFx64 func(msg any) []uint64
	AppendFx64 func(msg any, v uint64)

	GetRepSfx32 func(msg any) []int32
	AppendSfx32 func(msg any, v int32)

	GetRepSfx64 func(msg any) []int64
	AppendSfx64 func(msg any, v int64)

	GetRepF32 func(msg any) []float32
	AppendF32 func(msg any, v float32)

	GetRepF64 func(msg any) []float64
	AppendF64 func(msg any, v float64)

	GetI64 func(msg any) int64
	SetI64 func(msg any, v int64)

	GetU32 func(msg any) uint32
	SetU32 func(msg any, v uint32)

	GetU64 func(msg any) uint64
	SetU64 func(msg any, v uint64)

	GetS32 func(msg any) int32
	SetS32 func(msg any, v int32)

	GetS64 func(msg any) int64
	SetS64 func(msg any, v int64)

	GetFx32 func(msg any) uint32
	SetFx32 func(msg any, v uint32)

	GetFx64 func(msg any) uint64
	SetFx64 func(msg any, v uint64)

	GetSfx32 func(msg any) int32
	SetSfx32 func(msg any, v int32)

	GetSfx64 func(msg any) int64
	SetSfx64 func(msg any, v int64)

	GetF32 func(msg any) float32
	SetF32 func(msg any, v float32)

	GetF64 func(msg any) float64
	SetF64 func(msg any, v float64)

	GetBool    func(msg any) bool
	SetBool    func(msg any, v bool)
	GetRepBool func(msg any) []bool
	AppendBool func(msg any, v bool)

	GetStr    func(msg any) string
	SetStr    func(msg any, s string)
	GetRepStr func(msg any) []string
	AppendStr func(msg any, s string)

	GetBytes    func(msg any) []byte
	SetBytes    func(msg any, p []byte)
	GetRepBytes func(msg any) [][]byte
	AppendBytes func(msg any, p []byte)

	GetEnum    func(msg any) int32
	SetEnum    func(msg any, v int32)
	GetRepEnum func(msg any) []int32
	AppendEnum func(msg any, v int32)

	GetMsg         func(msg any) any // nil when absent
	GetOrCreateMsg func(msg any) any // allocates the singular submessage on demand (merge)
	RepMsgLen      func(msg any) int
	RepMsgAt       func(msg any, i int) any
	NewRepMsg      func(msg any) any // appends a fresh element, returns it to fill

	GetOptI32 func(msg any) *int32
	SetOptI32 func(msg any, v int32)

	GetOptI64 func(msg any) *int64
	SetOptI64 func(msg any, v int64)

	GetOptU32 func(msg any) *uint32
	SetOptU32 func(msg any, v uint32)

	GetOptU64 func(msg any) *uint64
	SetOptU64 func(msg any, v uint64)

	GetOptS32 func(msg any) *int32
	SetOptS32 func(msg any, v int32)

	GetOptS64 func(msg any) *int64
	SetOptS64 func(msg any, v int64)

	GetOptFx32 func(msg any) *uint32
	SetOptFx32 func(msg any, v uint32)

	GetOptFx64 func(msg any) *uint64
	SetOptFx64 func(msg any, v uint64)

	GetOptSfx32 func(msg any) *int32
	SetOptSfx32 func(msg any, v int32)

	GetOptSfx64 func(msg any) *int64
	SetOptSfx64 func(msg any, v int64)

	GetOptF32 func(msg any) *float32
	SetOptF32 func(msg any, v float32)

	GetOptF64 func(msg any) *float64
	SetOptF64 func(msg any, v float64)

	GetOptBool func(msg any) *bool
	SetOptBool func(msg any, v bool)

	GetOptStr func(msg any) *string
	SetOptStr func(msg any, s string)

	GetOptBytes func(msg any) *[]byte
	SetOptBytes func(msg any, p []byte)

	GetOptEnum func(msg any) *int32
	SetOptEnum func(msg any, v int32)

	// Oneof members carry their presence in the wrapper: GetOneof reports
	// the member value and whether the wrapper is set (so zero and empty
	// still encode), SetOneof builds the wrapper (last occurrence wins),
	// NewMsg allocates a fresh message member for the wrapper to hold.
	GetOneof func(msg any) (any, bool)
	SetOneof func(msg any, v any)
	NewMsg   func() any
}

// Table is one message's table: its rows plus a ByNum index sized to the
// message's max field number for O(1) decode lookup. The two hooks are
// optional: unset, unknown fields are dropped exactly as before.
type Table struct {
	Name   string
	Fields []*Field
	ByNum  []*Field

	// Unknown receives every field the table does not declare (or a known
	// field carrying the wrong wire type), as bytes including the tag.
	Unknown func(msg any, raw []byte)
	// EncodeUnknown returns the bytes a nested value retained; the parent
	// appends them after that value's known fields, inside its length prefix.
	EncodeUnknown func(msg any) []byte

	// encCap is the capacity the last encode needed: a size hint for the next
	// one, kept on the table so generated models carry no codec state.
	encCap atomic.Int64

	// hasFast reports whether any row carries valid offset columns. The
	// engine reads it once per message instead of scanning rows per field.
	hasFast bool
}

// capHint reports the last successful encode size plus headroom, or 0 before
// the first call. It is a size, deliberately not a capacity: one table serves
// every payload size of its message, so memorizing a large capacity would
// force every later small encode to allocate and zero it.
func (t *Table) capHint() int { return int(t.encCap.Load()) }

// setCapHint records what the next presize should be.
func (t *Table) setCapHint(n int) { t.encCap.Store(int64(n)) }

// NewTable builds the ByNum index over fields. Tables are read-only after
// this call; decode only reads them. Sub tables stay lazy (a func) so a
// message containing itself cannot form a package-init cycle.
func NewTable(name string, fields []*Field) *Table {
	max := 0
	for _, f := range fields {
		if int(f.Num) > max {
			max = int(f.Num)
		}
	}
	byNum := make([]*Field, max+1)
	for _, f := range fields {
		byNum[f.Num] = f
	}
	tb := &Table{Name: name, Fields: fields, ByNum: byNum}
	for _, f := range fields {
		if f.Fast {
			tb.hasFast = true
			break
		}
	}
	return tb
}

// lookup returns the row for num or nil for an unknown field.
func lookup(t *Table, num uint32) *Field {
	if int(num) < len(t.ByNum) {
		return t.ByNum[num]
	}
	return nil
}

// packable reports whether kind uses a varint/fixed primitive that packs.
func packableKind(k Kind) bool {
	switch k {
	case KindInt32, KindInt64, KindUint32, KindUint64,
		KindSint32, KindSint64, KindFixed32, KindFixed64,
		KindSfixed32, KindSfixed64, KindFloat, KindDouble,
		KindBool, KindEnum:
		return true
	}
	return false
}

// EncodeTable walks any table calling getters and writes straight into the
// encoder. Rows run in table order, so generated tables list fields in
// ascending field-number order for byte-identical output.
func EncodeTable(e *Encoder, msg any, t *Table, depth int) error {
	if err := CheckDepth(depth); err != nil {
		return err
	}
	if msg == nil {
		return nil
	}
	base := fastBase(t, msg)
	for _, f := range t.Fields {
		if base != nil && f.Fast {
			done, err := encodeFast(e, base, f, depth)
			if err != nil {
				return err
			}
			if done {
				continue
			}
		}
		if f.Repeated {
			if err := encodeRepeated(e, msg, f, depth); err != nil {
				return err
			}
			continue
		}
		if f.Oneof {
			v, ok := f.GetOneof(msg)
			if !ok {
				continue
			}
			if err := writeOneofValue(e, f, v, depth); err != nil {
				return err
			}
			continue
		}
		if f.Group {
			if err := encodeGroup(e, msg, f, depth); err != nil {
				return err
			}
			continue
		}
		if f.Foreign != nil {
			sub := f.GetMsg(msg)
			if sub == nil {
				continue
			}
			subBytes, err := f.Foreign.Encode(sub)
			if err != nil {
				return err
			}
			e.Tag(f.Num, WireLen)
			e.Data(subBytes)
			continue
		}
		switch f.Kind {
		case KindInt32:
			if f.Explicit {
				p := f.GetOptI32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(uint64(int64(*p)))
				continue
			}
			v := f.GetI32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		case KindInt64:
			if f.Explicit {
				p := f.GetOptI64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(uint64(*p))
				continue
			}
			v := f.GetI64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		case KindUint32:
			if f.Explicit {
				p := f.GetOptU32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(uint64(*p))
				continue
			}
			v := f.GetU32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		case KindUint64:
			if f.Explicit {
				p := f.GetOptU64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(*p)
				continue
			}
			v := f.GetU64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(v)
		case KindSint32:
			if f.Explicit {
				p := f.GetOptS32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(ZigZagEncode32(*p))
				continue
			}
			v := f.GetS32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode32(v))
		case KindSint64:
			if f.Explicit {
				p := f.GetOptS64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(ZigZagEncode64(*p))
				continue
			}
			v := f.GetS64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode64(v))
		case KindFixed32:
			if f.Explicit {
				p := f.GetOptFx32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed32)
				e.Fixed32(*p)
				continue
			}
			v := f.GetFx32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(v)
		case KindFixed64:
			if f.Explicit {
				p := f.GetOptFx64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed64)
				e.Fixed64(*p)
				continue
			}
			v := f.GetFx64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(v)
		case KindSfixed32:
			if f.Explicit {
				p := f.GetOptSfx32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed32)
				e.Fixed32(uint32(*p))
				continue
			}
			v := f.GetSfx32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(uint32(v))
		case KindSfixed64:
			if f.Explicit {
				p := f.GetOptSfx64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed64)
				e.Fixed64(uint64(*p))
				continue
			}
			v := f.GetSfx64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(uint64(v))
		case KindFloat:
			if f.Explicit {
				p := f.GetOptF32(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed32)
				e.Fixed32(math.Float32bits(*p))
				continue
			}
			v := f.GetF32(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(math.Float32bits(v))
		case KindDouble:
			if f.Explicit {
				p := f.GetOptF64(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireFixed64)
				e.Fixed64(math.Float64bits(*p))
				continue
			}
			v := f.GetF64(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(math.Float64bits(v))
		case KindBool:
			if f.Explicit {
				p := f.GetOptBool(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				if *p {
					e.Varint(1)
				} else {
					e.Varint(0)
				}
				continue
			}
			v := f.GetBool(msg)
			if !v && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
		case KindString:
			if f.Explicit {
				p := f.GetOptStr(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireLen)
				e.Text(*p)
				continue
			}
			v := f.GetStr(msg)
			if v == "" && !f.Required {
				continue
			}
			e.Tag(f.Num, WireLen)
			e.Text(v)
		case KindBytes:
			if f.Explicit {
				p := f.GetOptBytes(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireLen)
				e.Data(*p)
				continue
			}
			v := f.GetBytes(msg)
			if len(v) == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireLen)
			e.Data(v)
		case KindEnum:
			if f.Explicit {
				p := f.GetOptEnum(msg)
				if p == nil {
					continue
				}
				e.Tag(f.Num, WireVarint)
				e.Varint(uint64(int64(*p)))
				continue
			}
			v := f.GetEnum(msg)
			if v == 0 && !f.Required {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		case KindMessage:
			sub := f.GetMsg(msg)
			if sub == nil && !f.Required {
				continue
			}
			mark := e.Begin(f.Num)
			if err := EncodeTable(e, sub, f.Sub(), depth+1); err != nil {
				return err
			}
			e.End(mark)
		}
	}
	appendUnknown(e, t, msg)
	return nil
}

// encodeGroup frames one group value with start/end tags; the shared body
// is nil-tolerant, so required and oneof members need no extra check.
func encodeGroup(e *Encoder, msg any, f *Field, depth int) error {
	sub := f.GetMsg(msg)
	if sub == nil && !f.Required {
		return nil
	}
	e.Tag(f.Num, WireStartGroup)
	if err := EncodeTable(e, sub, f.Sub(), depth+1); err != nil {
		return err
	}
	e.Tag(f.Num, WireEndGroup)
	return nil
}

// writeOneofValue writes a set oneof member: no presence guard beyond the
// wrapper itself, so zero and empty values still encode.
func writeOneofValue(e *Encoder, f *Field, v any, depth int) error {
	switch f.Kind {
	case KindInt32:
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(int64(v.(int32))))
	case KindInt64:
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(v.(int64)))
	case KindUint32:
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(v.(uint32)))
	case KindUint64:
		e.Tag(f.Num, WireVarint)
		e.Varint(v.(uint64))
	case KindSint32:
		e.Tag(f.Num, WireVarint)
		e.Varint(ZigZagEncode32(v.(int32)))
	case KindSint64:
		e.Tag(f.Num, WireVarint)
		e.Varint(ZigZagEncode64(v.(int64)))
	case KindFixed32:
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(v.(uint32))
	case KindFixed64:
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(v.(uint64))
	case KindSfixed32:
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(uint32(v.(int32)))
	case KindSfixed64:
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(uint64(v.(int64)))
	case KindFloat:
		e.Tag(f.Num, WireFixed32)
		e.Fixed32(math.Float32bits(v.(float32)))
	case KindDouble:
		e.Tag(f.Num, WireFixed64)
		e.Fixed64(math.Float64bits(v.(float64)))
	case KindBool:
		e.Tag(f.Num, WireVarint)
		if v.(bool) {
			e.Varint(1)
		} else {
			e.Varint(0)
		}
	case KindString:
		e.Tag(f.Num, WireLen)
		e.Text(v.(string))
	case KindBytes:
		e.Tag(f.Num, WireLen)
		e.Data(v.([]byte))
	case KindEnum:
		e.Tag(f.Num, WireVarint)
		e.Varint(uint64(int64(v.(int32))))
	case KindMessage:
		if f.Group {
			e.Tag(f.Num, WireStartGroup)
			if err := EncodeTable(e, v, f.Sub(), depth+1); err != nil {
				return err
			}
			e.Tag(f.Num, WireEndGroup)
			return nil
		}
		if f.Foreign != nil {
			subBytes, err := f.Foreign.Encode(v)
			if err != nil {
				return err
			}
			e.Tag(f.Num, WireLen)
			e.Data(subBytes)
			return nil
		}
		mark := e.Begin(f.Num)
		if err := EncodeTable(e, v, f.Sub(), depth+1); err != nil {
			return err
		}
		e.End(mark)
	}
	return nil
}

// encodeRepeated writes one repeated row: packable numerics share one
// length prefix when the row packs, strings/bytes repeat the tag, messages
// recurse per element (a nil element encodes empty, like protoc-gen-go).
func encodeRepeated(e *Encoder, msg any, f *Field, depth int) error {
	switch f.Kind {
	case KindInt32:
		vs := f.GetRepI32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(int64(v)))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		}
	case KindInt64:
		vs := f.GetRepI64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		}
	case KindUint32:
		vs := f.GetRepU32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(v))
		}
	case KindUint64:
		vs := f.GetRepU64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(v)
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(v)
		}
	case KindSint32:
		vs := f.GetRepS32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(ZigZagEncode32(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode32(v))
		}
	case KindSint64:
		vs := f.GetRepS64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(ZigZagEncode64(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(ZigZagEncode64(v))
		}
	case KindFixed32:
		vs := f.GetRepFx32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(v)
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(v)
		}
	case KindFixed64:
		vs := f.GetRepFx64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(v)
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(v)
		}
	case KindSfixed32:
		vs := f.GetRepSfx32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(uint32(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(uint32(v))
		}
	case KindSfixed64:
		vs := f.GetRepSfx64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(uint64(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(uint64(v))
		}
	case KindFloat:
		vs := f.GetRepF32(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed32(math.Float32bits(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed32)
			e.Fixed32(math.Float32bits(v))
		}
	case KindDouble:
		vs := f.GetRepF64(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Fixed64(math.Float64bits(v))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireFixed64)
			e.Fixed64(math.Float64bits(v))
		}
	case KindBool:
		vs := f.GetRepBool(msg)
		if len(vs) == 0 {
			return nil
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
			return nil
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
		for _, v := range f.GetRepStr(msg) {
			e.Tag(f.Num, WireLen)
			e.Text(v)
		}
	case KindBytes:
		for _, v := range f.GetRepBytes(msg) {
			e.Tag(f.Num, WireLen)
			e.Data(v)
		}
	case KindEnum:
		vs := f.GetRepEnum(msg)
		if len(vs) == 0 {
			return nil
		}
		if f.Packed {
			mark := e.Begin(f.Num)
			for _, v := range vs {
				e.Varint(uint64(int64(v)))
			}
			e.End(mark)
			return nil
		}
		for _, v := range vs {
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		}
	case KindMessage:
		n := f.RepMsgLen(msg)
		for i := 0; i < n; i++ {
			sub := f.RepMsgAt(msg, i)
			if f.Foreign != nil {
				subBytes, err := f.Foreign.Encode(sub)
				if err != nil {
					return err
				}
				e.Tag(f.Num, WireLen)
				e.Data(subBytes)
				continue
			}
			if f.Group {
				e.Tag(f.Num, WireStartGroup)
				if err := EncodeTable(e, sub, f.Sub(), depth+1); err != nil {
					return err
				}
				e.Tag(f.Num, WireEndGroup)
				continue
			}
			mark := e.Begin(f.Num)
			// A nil element encodes empty, like protoc-gen-go: the
			// table closures normalize typed nils to nil, and
			// EncodeTable is nil-tolerant.
			if err := EncodeTable(e, sub, f.Sub(), depth+1); err != nil {
				return err
			}
			e.End(mark)
		}
	}
	return nil
}

// DecodeTable loops Decoder.Next, looks the field up by number in the
// indexed array, calls setters, and skips unknowns via Decoder.Skip.
// Singular scalars overwrite (last wins); singular submessages merge via
// GetOrCreateMsg; repeated numerics accept packed runs and unpacked
// singles; enums keep unknown values as int32.
func DecodeTable(d *Decoder, a *Arena, msg any, t *Table, depth int) error {
	if err := CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		f := lookup(t, num)
		if f == nil {
			if err := skipUnknown(t, d, msg, num, wt, depth); err != nil {
				return err
			}
			continue
		}
		if err := decodeField(d, a, t, msg, f, num, wt, depth); err != nil {
			return err
		}
	}
	return nil
}

// decodeField decodes one field occurrence: repeated rows fan out, oneof
// members wrap (last wins), groups run their own loop, foreign messages
// cross through the sibling, everything else reads and assigns.
func decodeField(d *Decoder, a *Arena, t *Table, msg any, f *Field, num uint32, wt WireType, depth int) error {
	if f.Repeated {
		return decodeRepeated(d, a, t, msg, f, wt, depth)
	}
	if f.Oneof {
		v, ok, err := readOneofValue(d, a, f, wt, depth)
		if err != nil {
			return err
		}
		if !ok {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		f.SetOneof(msg, v)
		return nil
	}
	if f.Group {
		if wt != WireStartGroup {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		return decodeGroup(d, a, msg, f, depth)
	}
	if f.Foreign != nil {
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		target := f.GetOrCreateMsg(msg)
		return f.Foreign.Decode(target, raw)
	}
	switch f.Kind {
	case KindInt32:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptI32(msg, int32(v))
		} else if !storeFast(t, msg, f, uint64(int64(int32(v)))) {
			f.SetI32(msg, int32(v))
		}
	case KindInt64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptI64(msg, int64(v))
		} else if !storeFast(t, msg, f, v) {
			f.SetI64(msg, int64(v))
		}
	case KindUint32:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptU32(msg, uint32(v))
		} else if !storeFast(t, msg, f, uint64(uint32(v))) {
			f.SetU32(msg, uint32(v))
		}
	case KindUint64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptU64(msg, v)
		} else if !storeFast(t, msg, f, v) {
			f.SetU64(msg, v)
		}
	case KindSint32:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptS32(msg, ZigZagDecode32(v))
		} else if !storeFast(t, msg, f, uint64(int64(ZigZagDecode32(v)))) {
			f.SetS32(msg, ZigZagDecode32(v))
		}
	case KindSint64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptS64(msg, ZigZagDecode64(v))
		} else if !storeFast(t, msg, f, uint64(ZigZagDecode64(v))) {
			f.SetS64(msg, ZigZagDecode64(v))
		}
	case KindFixed32:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptFx32(msg, v)
		} else if !storeFast(t, msg, f, uint64(v)) {
			f.SetFx32(msg, v)
		}
	case KindFixed64:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptFx64(msg, v)
		} else if !storeFast(t, msg, f, v) {
			f.SetFx64(msg, v)
		}
	case KindSfixed32:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptSfx32(msg, int32(v))
		} else if !storeFast(t, msg, f, uint64(int64(int32(v)))) {
			f.SetSfx32(msg, int32(v))
		}
	case KindSfixed64:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptSfx64(msg, int64(v))
		} else if !storeFast(t, msg, f, v) {
			f.SetSfx64(msg, int64(v))
		}
	case KindFloat:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptF32(msg, math.Float32frombits(v))
		} else if !storeFast(t, msg, f, uint64(v)) {
			f.SetF32(msg, math.Float32frombits(v))
		}
	case KindDouble:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptF64(msg, math.Float64frombits(v))
		} else if !storeFast(t, msg, f, v) {
			f.SetF64(msg, math.Float64frombits(v))
		}
	case KindBool:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptBool(msg, v != 0)
		} else if !storeFast(t, msg, f, v) {
			f.SetBool(msg, v != 0)
		}
	case KindString:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptStr(msg, a.AddString(raw))
		} else {
			f.SetStr(msg, a.AddString(raw))
		}
	case KindBytes:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptBytes(msg, a.AddBytes(raw))
		} else {
			f.SetBytes(msg, a.AddBytes(raw))
		}
	case KindEnum:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			f.SetOptEnum(msg, int32(v))
		} else if !storeFast(t, msg, f, uint64(int64(int32(v)))) {
			f.SetEnum(msg, int32(v))
		}
	case KindMessage:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		sub := Decoder{Buf: raw}
		target := f.GetOrCreateMsg(msg)
		if err := DecodeTable(&sub, a, target, f.Sub(), depth+1); err != nil {
			return err
		}
		if !sub.Done() {
			return ErrWire
		}
	}
	return nil
}

// decodeGroup runs a proto2 group body: members loop until the matching
// end tag, dispatched through the sub-table like a nested message.
func decodeGroup(d *Decoder, a *Arena, msg any, f *Field, depth int) error {
	return decodeGroupBody(d, a, f.GetOrCreateMsg(msg), f.Sub(), f.Num, depth)
}

// decodeGroupBody is the end-tag-terminated loop shared by plain and oneof
// group members: every member occurrence dispatches through sub like a
// nested message body.
func decodeGroupBody(d *Decoder, a *Arena, target any, sub *Table, group uint32, depth int) error {
	if err := CheckDepth(depth); err != nil {
		return err
	}
	for {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		if wt == WireEndGroup {
			if num != group {
				return ErrWire
			}
			return nil
		}
		g := lookup(sub, num)
		if g == nil {
			if err := skipUnknown(sub, d, target, num, wt, depth); err != nil {
				return err
			}
			continue
		}
		if err := decodeField(d, a, sub, target, g, num, wt, depth); err != nil {
			return err
		}
	}
}

// readOneofValue reads one set oneof member: the wire type must match (or
// the occurrence skips, like any unknown), then the value comes back
// unwrapped for SetOneof to wrap. ok=false means skip.
func readOneofValue(d *Decoder, a *Arena, f *Field, wt WireType, depth int) (v any, ok bool, err error) {
	switch f.Kind {
	case KindInt32:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return int32(v), true, nil
	case KindInt64:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return int64(v), true, nil
	case KindUint32:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return uint32(v), true, nil
	case KindUint64:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return v, true, nil
	case KindSint32:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return ZigZagDecode32(v), true, nil
	case KindSint64:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return ZigZagDecode64(v), true, nil
	case KindFixed32:
		if wt != WireFixed32 {
			return nil, false, nil
		}
		v, err := d.Fixed32()
		if err != nil {
			return nil, false, err
		}
		return v, true, nil
	case KindFixed64:
		if wt != WireFixed64 {
			return nil, false, nil
		}
		v, err := d.Fixed64()
		if err != nil {
			return nil, false, err
		}
		return v, true, nil
	case KindSfixed32:
		if wt != WireFixed32 {
			return nil, false, nil
		}
		v, err := d.Fixed32()
		if err != nil {
			return nil, false, err
		}
		return int32(v), true, nil
	case KindSfixed64:
		if wt != WireFixed64 {
			return nil, false, nil
		}
		v, err := d.Fixed64()
		if err != nil {
			return nil, false, err
		}
		return int64(v), true, nil
	case KindFloat:
		if wt != WireFixed32 {
			return nil, false, nil
		}
		v, err := d.Fixed32()
		if err != nil {
			return nil, false, err
		}
		return math.Float32frombits(v), true, nil
	case KindDouble:
		if wt != WireFixed64 {
			return nil, false, nil
		}
		v, err := d.Fixed64()
		if err != nil {
			return nil, false, err
		}
		return math.Float64frombits(v), true, nil
	case KindBool:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return v != 0, true, nil
	case KindString:
		if wt != WireLen {
			return nil, false, nil
		}
		raw, err := d.Bytes()
		if err != nil {
			return nil, false, err
		}
		return a.AddString(raw), true, nil
	case KindBytes:
		if wt != WireLen {
			return nil, false, nil
		}
		raw, err := d.Bytes()
		if err != nil {
			return nil, false, err
		}
		return a.AddBytes(raw), true, nil
	case KindEnum:
		if wt != WireVarint {
			return nil, false, nil
		}
		v, err := d.Varint()
		if err != nil {
			return nil, false, err
		}
		return int32(v), true, nil
	case KindMessage:
		if f.Group {
			if wt != WireStartGroup {
				return nil, false, nil
			}
			target := f.NewMsg()
			if err := decodeGroupBody(d, a, target, f.Sub(), f.Num, depth); err != nil {
				return nil, false, err
			}
			return target, true, nil
		}
		if wt != WireLen {
			return nil, false, nil
		}
		raw, err := d.Bytes()
		if err != nil {
			return nil, false, err
		}
		sub := Decoder{Buf: raw}
		if f.Foreign != nil {
			target := f.NewMsg()
			if err := f.Foreign.Decode(target, raw); err != nil {
				return nil, false, err
			}
			return target, true, nil
		}
		target := f.NewMsg()
		if err := DecodeTable(&sub, a, target, f.Sub(), depth+1); err != nil {
			return nil, false, err
		}
		if !sub.Done() {
			return nil, false, ErrWire
		}
		return target, true, nil
	}
	return nil, false, nil
}

// decodeRepeated handles one repeated row. Packable numerics accept both
// packed runs (wt == Len) and unpacked singles; strings, bytes and
// messages require Len.
func decodeRepeated(d *Decoder, a *Arena, t *Table, msg any, f *Field, wt WireType, depth int) error {
	switch f.Kind {
	case KindInt32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendI32(msg, int32(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendI32(msg, int32(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindInt64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendI64(msg, int64(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendI64(msg, int64(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindUint32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendU32(msg, uint32(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendU32(msg, uint32(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindUint64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendU64(msg, v)
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendU64(msg, v)
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindSint32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendS32(msg, ZigZagDecode32(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendS32(msg, ZigZagDecode32(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindSint64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendS64(msg, ZigZagDecode64(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendS64(msg, ZigZagDecode64(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindFixed32:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			f.AppendFx32(msg, v)
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				f.AppendFx32(msg, v)
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindFixed64:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			f.AppendFx64(msg, v)
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				f.AppendFx64(msg, v)
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindSfixed32:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			f.AppendSfx32(msg, int32(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				f.AppendSfx32(msg, int32(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindSfixed64:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			f.AppendSfx64(msg, int64(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				f.AppendSfx64(msg, int64(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindFloat:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			f.AppendF32(msg, math.Float32frombits(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				f.AppendF32(msg, math.Float32frombits(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindDouble:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			f.AppendF64(msg, math.Float64frombits(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				f.AppendF64(msg, math.Float64frombits(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindBool:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendBool(msg, v != 0)
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendBool(msg, v != 0)
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindString:
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		f.AppendStr(msg, a.AddString(raw))
	case KindBytes:
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		f.AppendBytes(msg, a.AddBytes(raw))
	case KindEnum:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			f.AppendEnum(msg, int32(v))
		case WireLen:
			if !packableKind(f.Kind) {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				f.AppendEnum(msg, int32(v))
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
	case KindMessage:
		if f.Group {
			if wt != WireStartGroup {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			if f.Foreign != nil {
				return skipUnknown(t, d, msg, f.Num, wt, depth)
			}
			item := f.NewRepMsg(msg)
			return decodeGroupBody(d, a, item, f.Sub(), f.Num, depth)
		}
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		if f.Foreign != nil {
			target := f.NewRepMsg(msg)
			if err := f.Foreign.Decode(target, raw); err != nil {
				return err
			}
			return nil
		}
		sub := Decoder{Buf: raw}
		item := f.NewRepMsg(msg)
		if err := DecodeTable(&sub, a, item, f.Sub(), depth+1); err != nil {
			return err
		}
		if !sub.Done() {
			return ErrWire
		}
	}
	return nil
}

// appendUnknown emits the bytes this value retained, after its own known
// fields. A nested call runs inside the parent's length prefix (or group
// frame), so the parent needs no separate hook.
func appendUnknown(e *Encoder, t *Table, msg any) {
	if t != nil && t.EncodeUnknown != nil {
		e.Buf = append(e.Buf, t.EncodeUnknown(msg)...)
	}
}

// skipUnknown drops one field the table does not accept, retaining its
// tag-inclusive bytes when the table has an Unknown hook. Without a hook this
// is exactly Decoder.Skip, so struct models keep dropping unknown fields.
func skipUnknown(t *Table, d *Decoder, msg any, num uint32, wt WireType, depth int) error {
	if t == nil || t.Unknown == nil {
		return d.Skip(num, wt, depth)
	}
	raw, err := d.SkipRaw(num, wt, depth)
	if err != nil {
		return err
	}
	t.Unknown(msg, raw)
	return nil
}
