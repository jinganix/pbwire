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
// type, extensions, groups-as-unknown) are skipped unless the table
// provides Unknown, in which case they are retained raw and replayed
// after the known fields in input order (generated models retain into
// unknownFields; hook-less hand tables drop them).
package runtime

import (
	"bytes"
	"math"
	"sort"
	"sync/atomic"
	"unicode/utf8"
	"unsafe"
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
	// UTF8 validates string payloads on decode: proto3 string fields reject
	// invalid UTF-8, proto2 strings and bytes never do, like the reference.
	// Set from the schema plan and the codegen emitter; tables lacking it
	// (hand-built or stale regen) skip validation.
	UTF8 bool
	// Fast and the columns below are struct-layout facts for the fast path.
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

	// DecFast marks rows the decode loop handles inline (see the gate in
	// DecodeTableLimits): plain Fast scalar singulars of covered kind.
	// Computed once in newTable from the static columns, so the hot loop
	// only tests the dynamic remainder (base, wire type, alignment).
	DecFast bool

	// subMemo caches the Sub resolution: the func is pure per field
	// (generated tables return a static table, dynamic ones memoize
	// under their own lock), while decode hits it per occurrence (100x
	// for a 100-element repeated message). First use pays the indirect
	// call, the rest pay one atomic load. Race-clean by construction:
	// concurrent first uses compute the same pure value.
	subMemo atomic.Pointer[Table]

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

	// Entry marks key/value rows of map-entry tables: they emit whenever
	// present, even for zero values, matching reference map semantics
	// (an unset entry key encodes as absent, like the C core). Present
	// reports the row's presence; the dynamic core reads the slot, while
	// generated rows predate presence tracking and always emit through
	// the Required mark NewTable sets instead (Entry stays false there).
	Entry   bool
	Present func(msg any) bool
}

// Table is one message's table: its rows plus a ByNum index for O(1) decode
// lookup. The index is a slice sized to the message's max field number,
// unless that number is far above the field count (a legal 536870911 would
// ask for gigabytes); such a table keeps a map instead. The two hooks are
// optional: unset, unknown fields are dropped exactly as before.
type Table struct {
	Name   string
	Fields []*Field
	ByNum  []*Field
	sparse map[uint32]*Field

	// fastTag dispatches 1-byte tags straight to their row: index is the
	// tag byte itself, so a hit proves field number and wire type at
	// once with no Next call, no lookup and no wire-type switch. Only
	// plain Fast scalar singulars with field numbers below 16 sit here
	// (built in newTable); everything else rides the generic path.
	fastTag [256]*Field

	// fastTag2 dispatches 2-byte tags (field numbers 16..2047) the same
	// way: index is the tag's first byte, fastTag2b holds the expected
	// second byte, and a hit proves field number and wire type at once
	// with no Next call and no lookup. First bytes shared by two tags
	// stay empty (built in newTable), so those tags keep the generic
	// path and can never mis-dispatch.
	fastTag2  [256]*Field
	fastTag2b [256]byte

	// Ext resolves an extension row for the dynamic core: binary decode
	// calls it only when the field number misses every row, so generated
	// and hand-built tables (nil hook) keep the old drop behavior and the
	// hot path pays one nil check per unknown field.
	Ext func(msg any, num uint32) *Field
	// Dynamic marks a plan-built table whose messages are *DMsg trees:
	// encodeSubmessage skips its generated-only fast path and the
	// type-assertion-free extension check below applies. Generated tables
	// leave it false, so repeated submessages avoid one assertion each.
	Dynamic bool
	// Unknown receives every field the table does not declare (or a known
	// field carrying the wrong wire type), as bytes including the tag.
	Unknown func(msg any, raw []byte)
	// EncodeUnknown returns the bytes a nested value retained; the parent
	// appends them after that value's known fields, inside its length prefix.
	EncodeUnknown func(msg any) []byte

	// encCap is the capacity the last encode needed: a size hint for the next
	// one, kept on the table so generated models carry no codec state.
	encCap atomic.Int64

	// arenaCap is the string/bytes payload the last decode stored: a size
	// hint for the next arena, so a steady workload allocates once at
	// roughly the size it uses instead of once at the input size. Same
	// learn-and-gate discipline as encCap: the store below is the only
	// shared write, and steady payloads must not rewrite it per decode.
	arenaCap atomic.Int64

	// hasFast reports whether any row carries valid offset columns. The
	// engine reads it once per message instead of scanning rows per field.
	hasFast bool

	// IsMapEntry marks a map-entry message table: holders repeat it and
	// sort their elements by key on the deterministic path, and the key
	// and value rows always emit, even when zero. The dynamic core takes
	// it from the schema plan; generated code asks for NewMapEntryTable.
	IsMapEntry bool
}

// capHint reports the last successful encode size plus headroom, or 0 before
// the first call. It is a size, deliberately not a capacity: one table serves
// every payload size of its message, so memorizing a large capacity would
// force every later small encode to allocate and zero it.
func (t *Table) capHint() int { return int(t.encCap.Load()) }

// setCapHint records what the next presize should be, but only once the size
// has moved by more than a factor of two. This store is the only shared write
// on a table, and its cache line also carries Fields/ByNum/Name, so a steady
// payload must not rewrite it per encode: that bounce is what stops parallel
// whole-message encodes from scaling.
func (t *Table) setCapHint(n int) {
	if cur := int(t.encCap.Load()); cur > 0 && n >= cur/2 && n <= cur*2 {
		return
	}
	t.encCap.Store(int64(n))
}

// arenaHint reports the payload bytes the last decode stored, or 0 before
// the first call. A nil table reports 0, like a table with no fast rows.
func (t *Table) arenaHint() int {
	if t == nil {
		return 0
	}
	return int(t.arenaCap.Load())
}

// setArenaHint records what the next arena presize should be, under the
// same factor-of-two gate as setCapHint: steady payloads never rewrite it,
// so parallel decodes share the line read-only. A nil table is a no-op.
func (t *Table) setArenaHint(n int) {
	if t == nil {
		return
	}
	if cur := int(t.arenaCap.Load()); cur > 0 && n >= cur/2 && n <= cur*2 {
		return
	}
	t.arenaCap.Store(int64(n))
}

// setCapHintOverflow and setArenaHintOverflow track a new max the moment
// the buffer outgrows its presize: without them a payload that grows
// within the factor-of-two band would reallocate on every call without the
// hint ever relearning. Overflow writes happen at most once per new max,
// so they stay off the steady path. A nil table is a no-op.
func (t *Table) setCapHintOverflow(n int) {
	if t == nil {
		return
	}
	t.encCap.Store(int64(n))
}

// setArenaHintOverflow is setCapHintOverflow for the decode arena.
func (t *Table) setArenaHintOverflow(n int) {
	if t == nil {
		return
	}
	t.arenaCap.Store(int64(n))
}

// NewTable builds the ByNum index over fields. Tables are read-only after
// this call; decode only reads them. Sub tables stay lazy (a func) so a
// message containing itself cannot form a package-init cycle.
func NewTable(name string, fields []*Field) *Table { return newTable(name, fields, false) }

// NewMapEntryTable is NewTable for a map-entry message: its key and value
// rows emit whenever the entry does, even for a zero key or an empty value,
// like the C core and every reference serializer.
func NewMapEntryTable(name string, fields []*Field) *Table {
	return newTable(name, fields, true)
}

func newTable(name string, fields []*Field, isMapEntry bool) *Table {
	max := 0
	for _, f := range fields {
		if int(f.Num) > max {
			max = int(f.Num)
		}
	}
	sparse := max > 1<<16 && max > 4*len(fields)
	var byNum []*Field
	if sparse {
		byNum = nil
	} else {
		byNum = make([]*Field, max+1)
	}
	var byMap map[uint32]*Field
	for _, f := range fields {
		if sparse {
			if byMap == nil {
				byMap = make(map[uint32]*Field, len(fields))
			}
			byMap[f.Num] = f
			continue
		}
		byNum[f.Num] = f
	}
	tb := &Table{Name: name, Fields: fields, ByNum: byNum, sparse: byMap, IsMapEntry: isMapEntry}
	// fastTag2 claims are counted first so a first byte shared by two
	// tags stays empty: at most one (field, second byte) per first byte,
	// and only single-claimed bytes install. An unpacked tag always
	// installs; the packed form installs for repeated packable kinds
	// (the same rows decodeRepeated accepts packed for), so a hit below
	// proves the exact tag the row would route to generically.
	var claimN [256]uint8
	var claimF [256]*Field
	var claimB [256]byte
	claim := func(tag uint64, f *Field) {
		if tag < 128 || tag > 16383 {
			return
		}
		b0 := byte(tag&0x7f) | 0x80
		b1 := byte(tag >> 7)
		if claimN[b0] == 1 && claimF[b0] == f && claimB[b0] == b1 {
			return
		}
		claimF[b0], claimB[b0] = f, b1
		if claimN[b0] < 2 {
			claimN[b0]++
		}
	}
	for _, f := range fields {
		claim(uint64(f.Num)<<3|uint64(f.Wire), f)
		if f.Repeated && packableKind(f.Kind) && f.Wire != WireLen {
			claim(uint64(f.Num)<<3|uint64(WireLen), f)
		}
	}
	for _, f := range fields {
		// DecFast folds the static half of the decode-loop gate (see
		// DecodeTableLimits): rows whose kind carries a real want-wire
		// entry and whose presence is plain. NewTable is the single
		// choke point for generated, hand-built and dynamic tables.
		f.DecFast = f.Fast && !f.Repeated && !f.Oneof && f.Foreign == nil &&
			!f.Group && !f.Explicit && fastWantWire[f.Kind] != 0xFF
		// fastTag admits the DecFast rows whose tag fits one byte
		// (field numbers below 16): the tag byte then identifies the
		// row exactly, so the loop skips tag parsing and lookup. Last
		// row wins, exactly like the ByNum index above.
		if f.DecFast && f.Num < 16 {
			tb.fastTag[byte(f.Num<<3)|byte(fastWantWire[f.Kind])] = f
		}
		if f.Fast {
			tb.hasFast = true
		}
	}
	if isMapEntry {
		if f := lookup(tb, 1); f != nil {
			f.Required = true
		}
		if f := lookup(tb, 2); f != nil {
			f.Required = true
		}
	}
	for b0 := 0; b0 < 256; b0++ {
		if claimN[b0] == 1 {
			tb.fastTag2[b0], tb.fastTag2b[b0] = claimF[b0], claimB[b0]
		}
	}
	return tb
}

// lookup returns the row for num or nil for an unknown field.
func lookup(t *Table, num uint32) *Field {
	if int(num) < len(t.ByNum) {
		return t.ByNum[num]
	}
	// A nil map read returns nil, so dense tables pay only the len check.
	return t.sparse[num]
}

// subTable resolves the row's Sub table once and memoizes it (see the
// subMemo column). A nil Sub stays nil and is never memoized, so rows
// without sub-tables keep their exact old behavior.
func (f *Field) subTable() *Table {
	if t := f.subMemo.Load(); t != nil {
		return t
	}
	t := f.Sub()
	if t != nil {
		f.subMemo.CompareAndSwap(nil, t)
	}
	return t
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

// skipZero reports whether an implicit row holding its zero value stays
// unwritten: required rows and present map-entry key/value rows emit even
// for zero, matching reference map semantics (an unset entry key encodes
// as absent, like the C core). The Present call runs only for entry rows,
// so plain fields pay one predictable branch on top of their old check.
func skipZero(f *Field, msg any) bool {
	if f.Entry {
		// Entry rows answer from presence alone: the Required mark
		// NewTable sets is for generated rows, which carry no presence.
		return f.Present == nil || !f.Present(msg)
	}
	if f.Required {
		return false
	}
	return true
}

// EncodeTable walks any table calling getters and writes straight into the
// encoder. Rows run in table order, so generated tables list fields in
// ascending field-number order for byte-identical output.
func EncodeTable(e *Encoder, msg any, t *Table, depth int) error {
	return encodeTable(e, msg, t, depth, false)
}

// encodeTable is the shared engine behind EncodeTable: deterministic selects
// the repeated-row writer once per message (never inside the per-field loop),
// so the plain path pays no per-field branch for a mode it never uses. Only
// map-entry message rows change under deterministic: a sorted copy encodes,
// never the caller's slice, and every other row keeps its order.
func encodeTable(e *Encoder, msg any, t *Table, depth int, deterministic bool) error {
	if err := CheckDepth(depth); err != nil {
		return err
	}
	if msg == nil {
		return nil
	}
	base := fastBase(t, msg)
	writeRep := encodeRepeatedPlain
	if deterministic {
		writeRep = encodeRepeatedDeterministic
	}
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
			if err := writeRep(e, msg, f, depth); err != nil {
				return err
			}
			continue
		}
		if f.Oneof {
			v, ok := f.GetOneof(msg)
			if !ok {
				continue
			}
			if err := writeOneofValue(e, f, v, depth, deterministic); err != nil {
				return err
			}
			continue
		}
		if f.Group {
			if err := encodeGroup(e, msg, f, depth, deterministic); err != nil {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
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
			// Negative zero is a value, not the default: compare bits so
			// -0.0 still emits, like the reference serializer.
			if math.Float32bits(v) == 0 && skipZero(f, msg) {
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
			// Negative zero is a value, not the default: compare bits so
			// -0.0 still emits, like the reference serializer.
			if math.Float64bits(v) == 0 && skipZero(f, msg) {
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
			if !v && skipZero(f, msg) {
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
			if v == "" && skipZero(f, msg) {
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
			if len(v) == 0 && skipZero(f, msg) {
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
			if v == 0 && skipZero(f, msg) {
				continue
			}
			e.Tag(f.Num, WireVarint)
			e.Varint(uint64(int64(v)))
		case KindMessage:
			sub := f.GetMsg(msg)
			if sub == nil && skipZero(f, msg) {
				continue
			}
			mark := e.Begin(f.Num)
			if err := encodeSubmessage(e, sub, f.subTable(), depth+1, deterministic); err != nil {
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
func encodeGroup(e *Encoder, msg any, f *Field, depth int, deterministic bool) error {
	sub := f.GetMsg(msg)
	if sub == nil && skipZero(f, msg) {
		return nil
	}
	e.Tag(f.Num, WireStartGroup)
	if err := encodeSubmessage(e, sub, f.subTable(), depth+1, deterministic); err != nil {
		return err
	}
	e.Tag(f.Num, WireEndGroup)
	return nil
}

// writeOneofValue writes a set oneof member: no presence guard beyond the
// wrapper itself, so zero and empty values still encode.
func writeOneofValue(e *Encoder, f *Field, v any, depth int, deterministic bool) error {
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
			if err := encodeSubmessage(e, v, f.subTable(), depth+1, deterministic); err != nil {
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
		if err := encodeSubmessage(e, v, f.subTable(), depth+1, deterministic); err != nil {
			return err
		}
		e.End(mark)
	}
	return nil
}

// encodeRepeatedPlain and encodeRepeatedDeterministic are the two row
// writers encodeTable selects between once per message: plain encode never
// touches the deterministic flag, not even as a parameter it ignores.
func encodeRepeatedPlain(e *Encoder, msg any, f *Field, depth int) error {
	return encodeRepeated(e, msg, f, depth, false)
}

func encodeRepeatedDeterministic(e *Encoder, msg any, f *Field, depth int) error {
	return encodeRepeated(e, msg, f, depth, true)
}

// encodeRepeated writes one repeated row: packable numerics share one
// length prefix when the row packs, strings/bytes repeat the tag, messages
// recurse per element (a nil element encodes empty, like protoc-gen-go).
// Under deterministic, repeated map-entry rows encode a copy sorted by key;
// the map check runs once per repeated message row, never per element.
func encodeRepeated(e *Encoder, msg any, f *Field, depth int, deterministic bool) error {
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
		// Resolve the sub-table once: Sub is pure per field (generated
		// tables return a static table, dynamic ones memoize under their
		// own lock), so resolving per element only repeats an indirect
		// call (plus a mutex on the dynamic path). Foreign rows carry no
		// Sub; otherwise the call count matches the old code exactly:
		// one resolution for non-empty rows plus the map-entry shape
		// check, which reuses it instead of calling Sub a second time.
		var subTab *Table
		if f.Foreign == nil && (n > 0 || (deterministic && !f.Group)) {
			subTab = f.subTable()
			if deterministic && !f.Group && isMapEntryTable(subTab) {
				return encodeMapEntriesSorted(e, msg, f, subTab, depth)
			}
		}
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
				if err := encodeSubmessage(e, sub, subTab, depth+1, deterministic); err != nil {
					return err
				}
				e.Tag(f.Num, WireEndGroup)
				continue
			}
			mark := e.Begin(f.Num)
			// A nil element encodes empty, like protoc-gen-go: the
			// table closures normalize typed nils to nil, and
			// encodeSubmessage is nil-tolerant.
			if err := encodeSubmessage(e, sub, subTab, depth+1, deterministic); err != nil {
				return err
			}
			e.End(mark)
		}
	}
	return nil
}

// isMapEntryTable reports whether sub is a map-entry message. The flag is
// explicit: NewMapEntryTable (generated code) and the dynamic core's plan
// set it, so an ordinary message that happens to look like an entry is
// never sorted or forced to emit zero keys.
func isMapEntryTable(sub *Table) bool {
	return sub != nil && sub.IsMapEntry
}

// mapKey is one entry's sort key: the raw numeric bits, the string bytes,
// or nothing when the key field is absent (or the table has no key row).
type mapKey struct {
	kind Kind
	has  bool
	i    int64
	u    uint64
	s    string
	p    []byte
}

// mapEntryKey reads an entry element's key through the entry table's own
// getters, so generated structs and dynamic trees share the comparison.
// A dynamic entry that never set its key sorts as missing.
func mapEntryKey(sub *Table, elem any) mapKey {
	var k mapKey
	if elem == nil || sub == nil || len(sub.ByNum) < 2 || sub.ByNum[1] == nil {
		return k
	}
	row := sub.ByNum[1]
	k.kind = row.Kind
	if dm, ok := elem.(*DMsg); ok {
		present := false
		for i, fp := range dm.typ.plan.Fields {
			if fp.Number == 1 {
				present = dm.slots[i].present
				break
			}
		}
		if !present {
			return k
		}
	}
	k.has = true
	switch row.Kind {
	case KindString:
		k.s = row.GetStr(elem)
	case KindBytes:
		k.p = row.GetBytes(elem)
	case KindInt32, KindSint32:
		k.i = int64(row.GetI32(elem))
	case KindSfixed32:
		k.i = int64(row.GetSfx32(elem))
	case KindEnum:
		k.i = int64(row.GetEnum(elem))
	case KindInt64, KindSint64:
		k.i = row.GetI64(elem)
	case KindSfixed64:
		k.i = row.GetSfx64(elem)
	case KindUint32:
		k.u = uint64(row.GetU32(elem))
	case KindFixed32:
		k.u = uint64(row.GetFx32(elem))
	case KindUint64:
		k.u = row.GetU64(elem)
	case KindFixed64:
		k.u = row.GetFx64(elem)
	case KindBool:
		if row.GetBool(elem) {
			k.u = 1
		}
	case KindFloat:
		k.u = uint64(math.Float32bits(row.GetF32(elem)))
	case KindDouble:
		k.u = math.Float64bits(row.GetF64(elem))
	default:
		k.has = false
	}
	return k
}

// cmpMapKeys orders two extracted entry keys, mirroring src/encode.c
// map_key_cmp and the Java GenericCodec deterministic branch: a missing
// key sorts first, string/bytes keys compare bytewise, signed numerics
// numerically, and unsigned kinds by raw bits.
func cmpMapKeys(ka, kb mapKey) int {
	if !ka.has || !kb.has {
		if ka.has == kb.has {
			return 0
		}
		if !ka.has {
			return -1
		}
		return 1
	}
	switch ka.kind {
	case KindString:
		if ka.s == kb.s {
			return 0
		}
		if ka.s < kb.s {
			return -1
		}
		return 1
	case KindBytes:
		return bytes.Compare(ka.p, kb.p)
	case KindInt32, KindSint32, KindSfixed32, KindEnum,
		KindInt64, KindSint64, KindSfixed64:
		if ka.i == kb.i {
			return 0
		}
		if ka.i < kb.i {
			return -1
		}
		return 1
	default:
		if ka.u == kb.u {
			return 0
		}
		if ka.u < kb.u {
			return -1
		}
		return 1
	}
}

// mapEntry orders one element with its extracted sort key: extracting
// once per element instead of once per comparison turns key reads from
// O(n log n) into O(n). The sort below stays stable, so equal keys keep
// their insertion order exactly as before.
type mapEntry struct {
	elem any
	key  mapKey
}

// encodeMapEntriesSorted encodes one repeated map-entry row in key order.
// Elements are copied into an index first: the caller's slice (and the
// dynamic tree's run) keep their insertion order. A table without a key
// row keeps insertion order, like the C core; a nil element sorts like a
// missing key and encodes empty, like the plain path. Entry bytes recurse
// deterministically, so nested maps sort at every level.
func encodeMapEntriesSorted(e *Encoder, msg any, f *Field, sub *Table, depth int) error {
	n := f.RepMsgLen(msg)
	if n == 0 {
		return nil
	}
	if len(sub.ByNum) < 2 || sub.ByNum[1] == nil {
		for i := 0; i < n; i++ {
			mark := e.Begin(f.Num)
			if err := encodeSubmessage(e, f.RepMsgAt(msg, i), sub, depth+1, true); err != nil {
				return err
			}
			e.End(mark)
		}
		return nil
	}
	order := make([]mapEntry, n)
	for i := 0; i < n; i++ {
		elem := f.RepMsgAt(msg, i)
		order[i] = mapEntry{elem: elem, key: mapEntryKey(sub, elem)}
	}
	sort.SliceStable(order, func(a, b int) bool {
		return cmpMapKeys(order[a].key, order[b].key) < 0
	})
	for _, ent := range order {
		mark := e.Begin(f.Num)
		if err := encodeSubmessage(e, ent.elem, sub, depth+1, true); err != nil {
			return err
		}
		e.End(mark)
	}
	return nil
}

// DecodeTable loops Decoder.Next, looks the field up by number in the
// indexed array, calls setters, and skips unknowns via Decoder.Skip.
// Singular scalars overwrite (last wins); singular submessages merge via
// GetOrCreateMsg; repeated numerics accept packed runs and unpacked
// singles; enums keep unknown values as int32.
func DecodeTable(d *Decoder, a *Arena, msg any, t *Table, depth int) error {
	return DecodeTableLimits(d, a, msg, t, depth, Limits{})
}

// DecodeTableLimits is DecodeTable with explicit bounds: inputs are already
// byte-checked by the caller, and nesting past the effective cap fails. The
// old name keeps its signature as a defaults wrapper. The limit rides by
// value and the depth check stays a single compare per message.
func DecodeTableLimits(d *Decoder, a *Arena, msg any, t *Table, depth int, lim Limits) error {
	if t == nil {
		return ErrNilTable
	}
	if err := lim.checkDepth(depth); err != nil {
		return err
	}
	// One address extraction per message: singular scalar stores below
	// reuse it instead of each storeFast recomputing it per field.
	base := fastBase(t, msg)
	for !d.Done() {
		// 1-byte tag direct dispatch (see fastTag): the tag byte proves
		// field number and wire type at once, so there is no Next call
		// and no lookup. Values parse with 1-byte varints inline and
		// fixed widths by call (both inline into this loop), and store
		// straight into the struct: zero calls on the common small-int
		// path. Anything uncovered — multi-byte tags, unknown fields,
		// unaligned 64-bit slots on 32-bit targets — falls through to
		// the generic path below, which is unchanged.
		if base != nil {
			if b := d.Buf[d.Off:]; len(b) > 0 && b[0] < 0x80 {
				if f := t.fastTag[b[0]]; f != nil {
					addr := unsafe.Add(base, uintptr(f.Off))
					if !fastWantAlign64[f.Kind] || aligned8(uintptr(addr)) {
						d.lastTag = d.Off
						d.Off++
						switch f.Kind {
						case KindInt32:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*int32)(addr) = int32(v)
							continue
						case KindInt64:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*int64)(addr) = int64(v)
							continue
						case KindUint32:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*uint32)(addr) = uint32(v)
							continue
						case KindUint64:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*uint64)(addr) = v
							continue
						case KindSint32:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*int32)(addr) = ZigZagDecode32(v)
							continue
						case KindSint64:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*int64)(addr) = ZigZagDecode64(v)
							continue
						case KindFixed32:
							v, err := d.Fixed32()
							if err != nil {
								return err
							}
							*(*uint32)(addr) = v
							continue
						case KindFixed64:
							v, err := d.Fixed64()
							if err != nil {
								return err
							}
							*(*uint64)(addr) = v
							continue
						case KindSfixed32:
							v, err := d.Fixed32()
							if err != nil {
								return err
							}
							*(*int32)(addr) = int32(v)
							continue
						case KindSfixed64:
							v, err := d.Fixed64()
							if err != nil {
								return err
							}
							*(*int64)(addr) = int64(v)
							continue
						case KindFloat:
							v, err := d.Fixed32()
							if err != nil {
								return err
							}
							*(*float32)(addr) = math.Float32frombits(v)
							continue
						case KindDouble:
							v, err := d.Fixed64()
							if err != nil {
								return err
							}
							*(*float64)(addr) = math.Float64frombits(v)
							continue
						case KindBool:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*bool)(addr) = v != 0
							continue
						case KindEnum:
							v, ok := d.varintByte()
							if !ok {
								var err error
								v, err = d.Varint()
								if err != nil {
									return err
								}
							}
							*(*int32)(addr) = int32(v)
							continue
						}
						// Unreachable: fastTag admits only the kinds
						// above, whose wire types the tag byte already
						// proved. Falling out of the switch would skip
						// the field exactly like decodeField's unmatched
						// Kind — never an abort, never a mis-decode.
						continue
					}
				}
			}
		}
		// 2-byte tag resolve (see fastTag2): the tag's first byte
		// indexes the row and the second byte verifies it, proving
		// field number and wire type with no Next call and no lookup.
		// Anything uncovered — 1-byte misses above, shared first
		// bytes, 3+-byte tags, short input — falls to Next below.
		// lastTag rides along exactly like Next sets it, so unknown
		// retention slices the same bytes either way.
		var f *Field
		var num uint32
		var wt WireType
		if base != nil {
			if b := d.Buf[d.Off:]; len(b) > 1 && b[0] >= 0x80 {
				if g := t.fastTag2[b[0]]; g != nil && b[1] == t.fastTag2b[b[0]] {
					v := uint64(b[0]&0x7f) | uint64(b[1])<<7
					num = uint32(v >> 3)
					wt = WireType(v & 7)
					d.lastTag = d.Off
					d.Off += 2
					f = g
				}
			}
		}
		if f == nil {
			n2, w2, err := d.Next()
			if err != nil {
				return err
			}
			num, wt = n2, w2
			f = lookup(t, num)
			if f == nil && t.Ext != nil {
				f = t.Ext(msg, num)
			}
			if f == nil {
				if err := skipUnknown(t, d, msg, num, wt, depth, lim); err != nil {
					return err
				}
				continue
			}
		}
		// Repeated unpacked occurrences inline here (see decodeRepeated
		// for the reference semantics): parse plus append with no
		// decodeField call and no decodeRepeated call. Packed runs
		// (wt == WireLen on packable numerics), groups and foreign rows
		// stay on the generic path: packed arrives as one tag per run
		// and rides the presized bulk loop there.
		if base != nil && f.Fast && f.Repeated && !f.Oneof && f.Foreign == nil &&
			!f.Group && !f.Explicit && wt != WireEndGroup {
			switch f.Kind {
			case KindInt32:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, int32(v)) {
						f.AppendI32(msg, int32(v))
					}
					continue
				}
			case KindInt64:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, int64(v)) {
						f.AppendI64(msg, int64(v))
					}
					continue
				}
			case KindUint32:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, uint32(v)) {
						f.AppendU32(msg, uint32(v))
					}
					continue
				}
			case KindUint64:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, v) {
						f.AppendU64(msg, v)
					}
					continue
				}
			case KindSint32:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, ZigZagDecode32(v)) {
						f.AppendS32(msg, ZigZagDecode32(v))
					}
					continue
				}
			case KindSint64:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, ZigZagDecode64(v)) {
						f.AppendS64(msg, ZigZagDecode64(v))
					}
					continue
				}
			case KindFixed32:
				if wt == WireFixed32 {
					v, err := d.Fixed32()
					if err != nil {
						return err
					}
					if !appendFast(base, f, v) {
						f.AppendFx32(msg, v)
					}
					continue
				}
			case KindFixed64:
				if wt == WireFixed64 {
					v, err := d.Fixed64()
					if err != nil {
						return err
					}
					if !appendFast(base, f, v) {
						f.AppendFx64(msg, v)
					}
					continue
				}
			case KindSfixed32:
				if wt == WireFixed32 {
					v, err := d.Fixed32()
					if err != nil {
						return err
					}
					if !appendFast(base, f, int32(v)) {
						f.AppendSfx32(msg, int32(v))
					}
					continue
				}
			case KindSfixed64:
				if wt == WireFixed64 {
					v, err := d.Fixed64()
					if err != nil {
						return err
					}
					if !appendFast(base, f, int64(v)) {
						f.AppendSfx64(msg, int64(v))
					}
					continue
				}
			case KindFloat:
				if wt == WireFixed32 {
					v, err := d.Fixed32()
					if err != nil {
						return err
					}
					if !appendFast(base, f, math.Float32frombits(v)) {
						f.AppendF32(msg, math.Float32frombits(v))
					}
					continue
				}
			case KindDouble:
				if wt == WireFixed64 {
					v, err := d.Fixed64()
					if err != nil {
						return err
					}
					if !appendFast(base, f, math.Float64frombits(v)) {
						f.AppendF64(msg, math.Float64frombits(v))
					}
					continue
				}
			case KindBool:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, v != 0) {
						f.AppendBool(msg, v != 0)
					}
					continue
				}
			case KindString:
				if wt == WireLen {
					raw, ok := d.bytesFast1()
					if !ok {
						var err error
						raw, err = d.Bytes()
						if err != nil {
							return err
						}
					}
					if f.UTF8 && !utf8.Valid(raw) {
						return ErrUTF8
					}
					presizeFast[string](base, f, unpackedNeed(len(d.Buf)-d.Off))
					rs := a.AddString(raw)
					if !appendFast(base, f, rs) {
						f.AppendStr(msg, rs)
					}
					continue
				}
			case KindBytes:
				if wt == WireLen {
					raw, ok := d.bytesFast1()
					if !ok {
						var err error
						raw, err = d.Bytes()
						if err != nil {
							return err
						}
					}
					presizeFast[[]byte](base, f, unpackedNeed(len(d.Buf)-d.Off))
					rb := a.AddBytes(raw)
					if !appendFast(base, f, rb) {
						f.AppendBytes(msg, rb)
					}
					continue
				}
			case KindEnum:
				if wt == WireVarint {
					v, ok := d.varintByte()
					if !ok {
						var err error
						v, err = d.Varint()
						if err != nil {
							return err
						}
					}
					if !appendFast(base, f, int32(v)) {
						f.AppendEnum(msg, int32(v))
					}
					continue
				}
			case KindMessage:
				if wt == WireLen {
					raw, ok := d.bytesFast1()
					if !ok {
						var err error
						raw, err = d.Bytes()
						if err != nil {
							return err
						}
					}
					sub := Decoder{Buf: raw}
					item := f.NewRepMsg(msg)
					if err := DecodeTableLimits(&sub, a, item, f.subTable(), depth+1, lim); err != nil {
						return err
					}
					if !sub.Done() {
						return ErrWire
					}
					continue
				}
			}
		}
		// Plain Fast scalar singulars stay in this loop: a flat Kind
		// switch does parse, transform and store with no function call,
		// no presence pre-branches and no nested dispatch. The gate is
		// branch-only; uncovered rows pay almost nothing. 64-bit slots
		// pre-check alignment because 32-bit targets may pack them
		// unaligned, and those rows stay generic. Bodies mirror
		// decodeField's singular cases exactly; wrong wire types never
		// reach here (they stay dropped as unknown data below).
		if f.DecFast && base != nil && wt == fastWantWire[f.Kind] &&
			(!fastWantAlign64[f.Kind] || aligned8(uintptr(unsafe.Add(base, uintptr(f.Off))))) {
			switch f.Kind {
			case KindInt32:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(int64(int32(v))))
			case KindInt64:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindUint32:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(uint32(v)))
			case KindUint64:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindSint32:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(int64(ZigZagDecode32(v))))
			case KindSint64:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(ZigZagDecode64(v)))
			case KindFixed32:
				v, err := d.Fixed32()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(v))
			case KindFixed64:
				v, err := d.Fixed64()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindSfixed32:
				v, err := d.Fixed32()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(int64(int32(v))))
			case KindSfixed64:
				v, err := d.Fixed64()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindFloat:
				v, err := d.Fixed32()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(v))
			case KindDouble:
				v, err := d.Fixed64()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindBool:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, v)
			case KindEnum:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				storeFast(base, f, uint64(int64(int32(v))))
			}
			// No default: the gate admits only the 14 kinds above (the
			// rest carry the 0xFF sentinel); an out-of-range Kind falls
			// out of the switch and is skipped exactly like decodeField's
			// unmatched Kind — never an abort, never a mis-decode.
			continue
		}
		if err := decodeField(d, a, t, msg, base, f, num, wt, depth, lim); err != nil {
			return err
		}
	}
	return nil
}

// decodeField decodes one field occurrence: repeated rows fan out, oneof
// members wrap (last wins), groups run their own loop, foreign messages
// cross through the sibling, everything else reads and assigns.
func decodeField(d *Decoder, a *Arena, t *Table, msg any, base unsafe.Pointer, f *Field, num uint32, wt WireType, depth int, lim Limits) error {
	if f.Repeated {
		return decodeRepeated(d, a, t, msg, base, f, wt, depth, lim)
	}
	if f.Oneof {
		v, ok, err := readOneofValue(d, a, f, wt, depth, lim)
		if err != nil {
			return err
		}
		if !ok {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		f.SetOneof(msg, v)
		return nil
	}
	if f.Group {
		if wt != WireStartGroup {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		return decodeGroup(d, a, msg, f, depth, lim)
	}
	if f.Foreign != nil {
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
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
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, int32(v)) {
				f.SetOptI32(msg, int32(v))
			}
		} else if !storeFast(base, f, uint64(int64(int32(v)))) {
			f.SetI32(msg, int32(v))
		}
	case KindInt64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, int64(v)) {
				f.SetOptI64(msg, int64(v))
			}
		} else if !storeFast(base, f, v) {
			f.SetI64(msg, int64(v))
		}
	case KindUint32:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, uint32(v)) {
				f.SetOptU32(msg, uint32(v))
			}
		} else if !storeFast(base, f, uint64(uint32(v))) {
			f.SetU32(msg, uint32(v))
		}
	case KindUint64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, v) {
				f.SetOptU64(msg, v)
			}
		} else if !storeFast(base, f, v) {
			f.SetU64(msg, v)
		}
	case KindSint32:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, ZigZagDecode32(v)) {
				f.SetOptS32(msg, ZigZagDecode32(v))
			}
		} else if !storeFast(base, f, uint64(int64(ZigZagDecode32(v)))) {
			f.SetS32(msg, ZigZagDecode32(v))
		}
	case KindSint64:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, ZigZagDecode64(v)) {
				f.SetOptS64(msg, ZigZagDecode64(v))
			}
		} else if !storeFast(base, f, uint64(ZigZagDecode64(v))) {
			f.SetS64(msg, ZigZagDecode64(v))
		}
	case KindFixed32:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, v) {
				f.SetOptFx32(msg, v)
			}
		} else if !storeFast(base, f, uint64(v)) {
			f.SetFx32(msg, v)
		}
	case KindFixed64:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, v) {
				f.SetOptFx64(msg, v)
			}
		} else if !storeFast(base, f, v) {
			f.SetFx64(msg, v)
		}
	case KindSfixed32:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, int32(v)) {
				f.SetOptSfx32(msg, int32(v))
			}
		} else if !storeFast(base, f, uint64(int64(int32(v)))) {
			f.SetSfx32(msg, int32(v))
		}
	case KindSfixed64:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, int64(v)) {
				f.SetOptSfx64(msg, int64(v))
			}
		} else if !storeFast(base, f, v) {
			f.SetSfx64(msg, int64(v))
		}
	case KindFloat:
		if wt != WireFixed32 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed32()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, math.Float32frombits(v)) {
				f.SetOptF32(msg, math.Float32frombits(v))
			}
		} else if !storeFast(base, f, uint64(v)) {
			f.SetF32(msg, math.Float32frombits(v))
		}
	case KindDouble:
		if wt != WireFixed64 {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Fixed64()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, math.Float64frombits(v)) {
				f.SetOptF64(msg, math.Float64frombits(v))
			}
		} else if !storeFast(base, f, v) {
			f.SetF64(msg, math.Float64frombits(v))
		}
	case KindBool:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, v != 0) {
				f.SetOptBool(msg, v != 0)
			}
		} else if !storeFast(base, f, v) {
			f.SetBool(msg, v != 0)
		}
	case KindString:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		raw, ok := d.bytesFast1()
		if !ok {
			var err error
			raw, err = d.Bytes()
			if err != nil {
				return err
			}
		}
		if f.UTF8 && !utf8.Valid(raw) {
			return ErrUTF8
		}
		str := a.AddString(raw)
		if !storeFastStr(base, f, str) {
			if f.Explicit {
				f.SetOptStr(msg, str)
			} else {
				f.SetStr(msg, str)
			}
		}
	case KindBytes:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		raw, ok := d.bytesFast1()
		if !ok {
			var err error
			raw, err = d.Bytes()
			if err != nil {
				return err
			}
		}
		blk := a.AddBytes(raw)
		if !storeFastBytes(base, f, blk) {
			if f.Explicit {
				f.SetOptBytes(msg, blk)
			} else {
				f.SetBytes(msg, blk)
			}
		}
	case KindEnum:
		if wt != WireVarint {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		v, err := d.Varint()
		if err != nil {
			return err
		}
		if f.Explicit {
			if !storeFastOpt(base, f, int32(v)) {
				f.SetOptEnum(msg, int32(v))
			}
		} else if !storeFast(base, f, uint64(int64(int32(v)))) {
			f.SetEnum(msg, int32(v))
		}
	case KindMessage:
		if wt != WireLen {
			return skipUnknown(t, d, msg, num, wt, depth, lim)
		}
		raw, ok := d.bytesFast1()
		if !ok {
			var err error
			raw, err = d.Bytes()
			if err != nil {
				return err
			}
		}
		sub := Decoder{Buf: raw}
		target := f.GetOrCreateMsg(msg)
		if err := DecodeTableLimits(&sub, a, target, f.subTable(), depth+1, lim); err != nil {
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
func decodeGroup(d *Decoder, a *Arena, msg any, f *Field, depth int, lim Limits) error {
	return decodeGroupBody(d, a, f.GetOrCreateMsg(msg), f.subTable(), f.Num, depth+1, lim)
}

// decodeGroupBody is the end-tag-terminated loop shared by plain and oneof
// group members: every member occurrence dispatches through sub like a
// nested message body.
func decodeGroupBody(d *Decoder, a *Arena, target any, sub *Table, group uint32, depth int, lim Limits) error {
	if err := lim.checkDepth(depth); err != nil {
		return err
	}
	base := fastBase(sub, target)
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
		if g == nil && sub.Ext != nil {
			g = sub.Ext(target, num)
		}
		if g == nil {
			if err := skipUnknown(sub, d, target, num, wt, depth, lim); err != nil {
				return err
			}
			continue
		}
		if err := decodeField(d, a, sub, target, base, g, num, wt, depth, lim); err != nil {
			return err
		}
	}
}

// readOneofValue reads one set oneof member: the wire type must match (or
// the occurrence skips, like any unknown), then the value comes back
// unwrapped for SetOneof to wrap. ok=false means skip.
func readOneofValue(d *Decoder, a *Arena, f *Field, wt WireType, depth int, lim Limits) (v any, ok bool, err error) {
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
		if f.UTF8 && !utf8.Valid(raw) {
			return nil, false, ErrUTF8
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
			if err := decodeGroupBody(d, a, target, f.subTable(), f.Num, depth+1, lim); err != nil {
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
		if err := DecodeTableLimits(&sub, a, target, f.subTable(), depth+1, lim); err != nil {
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
func decodeRepeated(d *Decoder, a *Arena, t *Table, msg any, base unsafe.Pointer, f *Field, wt WireType, depth int, lim Limits) error {
	switch f.Kind {
	case KindInt32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, int32(v)) {
				f.AppendI32(msg, int32(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedI32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, int32(v)) {
					f.AppendI32(msg, int32(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindInt64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, int64(v)) {
				f.AppendI64(msg, int64(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedI64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, int64(v)) {
					f.AppendI64(msg, int64(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindUint32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, uint32(v)) {
				f.AppendU32(msg, uint32(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[uint32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedU32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, uint32(v)) {
					f.AppendU32(msg, uint32(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindUint64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, v) {
				f.AppendU64(msg, v)
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[uint64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedU64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, v) {
					f.AppendU64(msg, v)
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindSint32:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, ZigZagDecode32(v)) {
				f.AppendS32(msg, ZigZagDecode32(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedS32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, ZigZagDecode32(v)) {
					f.AppendS32(msg, ZigZagDecode32(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindSint64:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, ZigZagDecode64(v)) {
				f.AppendS64(msg, ZigZagDecode64(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedS64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, ZigZagDecode64(v)) {
					f.AppendS64(msg, ZigZagDecode64(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindFixed32:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			if !appendFast(base, f, v) {
				f.AppendFx32(msg, v)
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[uint32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedFx32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				if !appendFast(base, f, v) {
					f.AppendFx32(msg, v)
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindFixed64:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			if !appendFast(base, f, v) {
				f.AppendFx64(msg, v)
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[uint64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedFx64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				if !appendFast(base, f, v) {
					f.AppendFx64(msg, v)
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindSfixed32:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			if !appendFast(base, f, int32(v)) {
				f.AppendSfx32(msg, int32(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedSfx32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				if !appendFast(base, f, int32(v)) {
					f.AppendSfx32(msg, int32(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindSfixed64:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			if !appendFast(base, f, int64(v)) {
				f.AppendSfx64(msg, int64(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedSfx64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				if !appendFast(base, f, int64(v)) {
					f.AppendSfx64(msg, int64(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindFloat:
		switch wt {
		case WireFixed32:
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			if !appendFast(base, f, math.Float32frombits(v)) {
				f.AppendF32(msg, math.Float32frombits(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[float32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedF32(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed32()
				if err != nil {
					return err
				}
				if !appendFast(base, f, math.Float32frombits(v)) {
					f.AppendF32(msg, math.Float32frombits(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindDouble:
		switch wt {
		case WireFixed64:
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			if !appendFast(base, f, math.Float64frombits(v)) {
				f.AppendF64(msg, math.Float64frombits(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[float64](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedF64(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Fixed64()
				if err != nil {
					return err
				}
				if !appendFast(base, f, math.Float64frombits(v)) {
					f.AppendF64(msg, math.Float64frombits(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindBool:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, v != 0) {
				f.AppendBool(msg, v != 0)
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[bool](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedBool(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, v != 0) {
					f.AppendBool(msg, v != 0)
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindString:
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		if f.UTF8 && !utf8.Valid(raw) {
			return ErrUTF8
		}
		rs := a.AddString(raw)
		presizeFast[string](base, f, unpackedNeed(len(d.Buf)-d.Off))
		if !appendFast(base, f, rs) {
			f.AppendStr(msg, rs)
		}
	case KindBytes:
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		rb := a.AddBytes(raw)
		presizeFast[[]byte](base, f, unpackedNeed(len(d.Buf)-d.Off))
		if !appendFast(base, f, rb) {
			f.AppendBytes(msg, rb)
		}
	case KindEnum:
		switch wt {
		case WireVarint:
			v, err := d.Varint()
			if err != nil {
				return err
			}
			if !appendFast(base, f, int32(v)) {
				f.AppendEnum(msg, int32(v))
			}
		case WireLen:
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := Decoder{Buf: raw}
			if base != nil && f.Fast {
				presizeFast[int32](base, f, packedNeed(f.Kind, len(raw)))
				return appendPackedEnum(base, f, &sub)
			}
			for !sub.Done() {
				v, err := sub.Varint()
				if err != nil {
					return err
				}
				if !appendFast(base, f, int32(v)) {
					f.AppendEnum(msg, int32(v))
				}
			}
		default:
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
		}
	case KindMessage:
		if f.Group {
			if wt != WireStartGroup {
				return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
			}
			if f.Foreign != nil {
				return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
			}
			item := f.NewRepMsg(msg)
			return decodeGroupBody(d, a, item, f.subTable(), f.Num, depth+1, lim)
		}
		if wt != WireLen {
			return skipUnknown(t, d, msg, f.Num, wt, depth, lim)
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
		if err := DecodeTableLimits(&sub, a, item, f.subTable(), depth+1, lim); err != nil {
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
func skipUnknown(t *Table, d *Decoder, msg any, num uint32, wt WireType, depth int, lim Limits) error {
	if t == nil || t.Unknown == nil {
		return d.SkipLimits(num, wt, depth, lim)
	}
	raw, err := d.SkipRawLimits(num, wt, depth, lim)
	if err != nil {
		return err
	}
	t.Unknown(msg, raw)
	return nil
}
