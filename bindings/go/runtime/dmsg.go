// Dynamic Go core: a value tree plus per-message tables built from a runtime
// schema plan, so a FileDescriptorSet alone is enough to encode and decode.
//
// The tree holds values, never wire facts: every tag, length, zigzag and
// packed run is decided by the shared EncodeTable/DecodeTable engine. The
// table closures below only move bits into slots, exactly like the generated
// struct closures move bits into struct fields.
//
// Ownership: a decoded DMsg owns one Arena for its strings and byte payloads,
// so the result stays valid after the caller reuses the input slice. Unknown
// fields are retained per message (bytes including the tag) and re-emitted
// after that message's known fields, matching the C core.
package runtime

import (
	"fmt"
	"math"
	"sync"

	"google.golang.org/protobuf/types/descriptorpb"
)

// slot is one field's value. The union covers the four shapes the wire has:
// scalar bits, a byte/string payload, a nested message, and a repeated run
// (any of the typed slices). opt holds the pointer an explicit-presence
// scalar getter hands back, oneof the value of a set oneof member.
type slot struct {
	present  bool
	u        uint64
	str      string
	p        []byte
	m        *DMsg
	rep      any
	opt      any
	oneof    any
	oneofSet bool
}

// DMsg is one dynamic message tree.
type DMsg struct {
	typ     *DType
	slots   []slot
	unknown []byte
	arena   *Arena
}

// DType is a message type bound to its schema plan and its runtime table.
// Types are cached per schema, so the same plan always gets the same tables.
type DType struct {
	s     *Schema
	plan  *MessagePlan
	once  sync.Once
	table *Table
	mu    sync.Mutex
	subs  map[int]*DType
}

// Schema.Type returns the dynamic type for a fully qualified message name.
func (s *Schema) Type(full string) (*DType, error) {
	if s == nil {
		return nil, fmt.Errorf("codec: nil schema")
	}
	if t, ok := s.types.Load(full); ok {
		return t.(*DType), nil
	}
	plan, err := s.Message(full)
	if err != nil {
		return nil, err
	}
	built := &DType{s: s, plan: plan}
	actual, _ := s.types.LoadOrStore(full, built)
	return actual.(*DType), nil
}

// Plan exposes the message plan the type was built from.
func (t *DType) Plan() *MessagePlan { return t.plan }

// table returns the message's runtime table, building and caching it on first
// use. Building is lazy so a recursive message cannot recurse forever while
// its own table is still being assembled.
func (t *DType) tableFor() *Table {
	t.once.Do(func() { t.table = t.buildTable() })
	return t.table
}

// sub resolves the dynamic type of a message-typed field.
func (t *DType) sub(ordinal int) (*DType, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if st, ok := t.subs[ordinal]; ok {
		return st, nil
	}
	fp := t.plan.Fields[ordinal]
	st, err := t.s.Type(fp.Message)
	if err != nil {
		return nil, err
	}
	if t.subs == nil {
		t.subs = map[int]*DType{}
	}
	t.subs[ordinal] = st
	return st, nil
}

// New builds an empty message of this type.
func (t *DType) New() *DMsg {
	return &DMsg{typ: t, slots: make([]slot, len(t.plan.Fields))}
}

// Decode parses wire bytes into a new tree.
func (t *DType) Decode(wire []byte) (*DMsg, error) {
	m := t.New()
	a := &Arena{}
	a.Grow(len(wire))
	m.arena = a
	d := Decoder{Buf: wire}
	if err := DecodeTable(&d, a, m, t.tableFor(), 0); err != nil {
		return nil, err
	}
	if !d.Done() {
		return nil, ErrWire
	}
	return m, nil
}

// Encode serializes the tree.
func (t *DType) Encode(m *DMsg) ([]byte, error) { return m.Encode() }

// Encode serializes the tree into a fresh slice the caller owns.
func (m *DMsg) Encode() ([]byte, error) {
	t := m.typ.tableFor()
	cap0 := t.capHint()
	if cap0 < 256 {
		cap0 = 256
	}
	e := Encoder{Buf: make([]byte, 0, cap0)}
	if err := EncodeTable(&e, m, t, 0); err != nil {
		return nil, err
	}
	t.setCapHint(len(e.Buf) + 64)
	return e.Result(), nil
}

// UnknownBytes returns the fields this message retained that its schema does
// not declare, as raw wire bytes including their tags.
func (m *DMsg) UnknownBytes() []byte { return m.unknown }

// buildTable turns the plan into the shared engine's row form. Rows land in
// field-number order (C4): the engine emits table order, and the wire is
// number-ordered, so declaration order would corrupt byte equality.
// Slot ordinals are captured per row and never change.
func (t *DType) buildTable() *Table {
	order := make([]int, 0, len(t.plan.Fields))
	for i := range t.plan.Fields {
		order = append(order, i)
	}
	for i := 1; i < len(order); i++ {
		j := i
		for j > 0 && t.plan.Fields[order[j-1]].Number > t.plan.Fields[order[j]].Number {
			order[j-1], order[j] = order[j], order[j-1]
			j--
		}
	}
	rows := make([]*Field, 0, len(t.plan.Fields))
	for _, i := range order {
		rows = append(rows, t.buildRow(i, t.plan.Fields[i]))
	}
	tb := NewTable(t.plan.Full, rows)
	tb.Unknown = func(msg any, raw []byte) {
		d := msg.(*DMsg)
		d.unknown = append(d.unknown, raw...)
	}
	tb.EncodeUnknown = func(msg any) []byte { return msg.(*DMsg).unknown }
	return tb
}

// kindOf maps a declared proto type onto the engine's scalar kind.
func kindOf(p descriptorpb.FieldDescriptorProto_Type) (Kind, bool) {
	switch p {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32:
		return KindInt32, true
	case descriptorpb.FieldDescriptorProto_TYPE_INT64:
		return KindInt64, true
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32:
		return KindUint32, true
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64:
		return KindUint64, true
	case descriptorpb.FieldDescriptorProto_TYPE_SINT32:
		return KindSint32, true
	case descriptorpb.FieldDescriptorProto_TYPE_SINT64:
		return KindSint64, true
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return KindFixed32, true
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		return KindFixed64, true
	case descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		return KindSfixed32, true
	case descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return KindSfixed64, true
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		return KindFloat, true
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		return KindDouble, true
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return KindBool, true
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		return KindString, true
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		return KindBytes, true
	case descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return KindEnum, true
	case descriptorpb.FieldDescriptorProto_TYPE_MESSAGE,
		descriptorpb.FieldDescriptorProto_TYPE_GROUP:
		return KindMessage, true
	}
	return 0, false
}

// explicitPresence reports whether the row carries presence as a pointer,
// which the engine's Opt* getters express. Singular messages and groups have
// presence too, but as a nil value, not a pointer.
func explicitPresence(fp *FieldPlan) bool {
	if fp.Repeated || fp.Presence == PresenceOneof || fp.Presence == PresenceRequired {
		return false
	}
	if fp.Presence != PresenceExplicit && fp.Presence != PresenceProto3Optional {
		return false
	}
	return fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group
}

func repOf[T any](s *slot) []T {
	if s.rep == nil {
		return nil
	}
	return s.rep.([]T)
}

func appendRep[T any](s *slot, v T) {
	if s.rep == nil {
		s.rep = []T{v}
		return
	}
	s.rep = append(s.rep.([]T), v)
}

// mergeOneof assigns a set oneof member and zeroes its siblings. It runs
// without any built table (buildTable may still be pending), so the siblings
// come from a plan scan, not a cache. When winner and newcomer are both a
// nested value tree (C2: split oneof occurrences share one message), the
// incoming tree merges in field by field instead of replacing the winner.
func mergeOneof(m *DMsg, i int, v any) {
	s := &m.slots[i]
	if old, ok := s.oneof.(*DMsg); ok && s.oneofSet {
		if in, ok := v.(*DMsg); ok {
			in.mergeInto(old)
			v = old
		}
	}
	s.present, s.oneof, s.oneofSet = true, v, true
	storeOneof(s, v)
	fp := m.typ.plan.Fields[i]
	for j, other := range m.typ.plan.Fields {
		if j == i || other.Presence != PresenceOneof || other.Oneof != fp.Oneof {
			continue
		}
		m.slots[j] = slot{}
	}
}

// mergeInto folds a second occurrence of the same submessage into this tree:
// scalars take the last value, repeated runs append, unknown bytes append.
func (m *DMsg) mergeInto(dst *DMsg) {
	if m == nil || dst == nil {
		return
	}
	for i := range m.slots {
		if !m.slots[i].present || m.slots[i].oneofSet {
			continue
		}
		if m.typ.plan.Fields[i].Repeated {
			if sel := m.slots[i].rep; sel != nil {
				dst.slots[i].present = true
				dst.slots[i].rep = appendRepeatedRep(dst.slots[i].rep, sel)
			}
			continue
		}
		dst.slots[i] = m.slots[i]
	}
	dst.unknown = append(dst.unknown, m.unknown...)
}

// the same rows serve every message of that plan.
func (t *DType) buildRow(i int, fp *FieldPlan) *Field {
	acc := func(msg any) *slot { return &msg.(*DMsg).slots[i] }
	f := &Field{
		Num:      fp.Number,
		Wire:     fp.Wire,
		Kind:     Kind(0),
		Explicit: explicitPresence(fp),
		Required: fp.Presence == PresenceRequired,
		Repeated: fp.Repeated,
		Packed:   fp.Packed,
		Group:    fp.Group,
		Oneof:    fp.Presence == PresenceOneof,
	}
	kind, ok := kindOf(fp.Proto)
	if !ok {
		return nil
	}
	f.Kind = kind
	if kind == KindMessage {
		f.Sub = func() *Table {
			st, err := t.sub(i)
			if err != nil {
				return nil
			}
			return st.tableFor()
		}
		f.GetMsg = func(msg any) any {
			if s := acc(msg).m; s != nil {
				return s
			}
			return nil
		}
		f.GetOrCreateMsg = func(msg any) any {
			s := acc(msg)
			if s.m == nil {
				st, err := t.sub(i)
				if err != nil || st == nil {
					return nil
				}
				s.m = st.New()
			}
			s.present = true
			return s.m
		}
		f.NewMsg = func() any {
			st, err := t.sub(i)
			if err != nil || st == nil {
				return nil
			}
			return st.New()
		}
		f.RepMsgLen = func(msg any) int {
			if s := acc(msg).rep; s != nil {
				return len(s.([]*DMsg))
			}
			return 0
		}
		f.RepMsgAt = func(msg any, idx int) any { return repOf[*DMsg](acc(msg))[idx] }
		f.NewRepMsg = func(msg any) any {
			s := acc(msg)
			st, err := t.sub(i)
			if err != nil || st == nil {
				return nil
			}
			child := st.New()
			s.present = true
			appendRep(s, child)
			return child
		}
	}
	if f.Oneof {
		f.GetOneof = func(msg any) (any, bool) {
			s := acc(msg)
			return s.oneof, s.oneofSet
		}
		f.SetOneof = func(msg any, v any) {
			mergeOneof(msg.(*DMsg), i, v)
		}
	}
	if f.Repeated {
		t.buildRepeatedRow(f, fp, acc)
		return f
	}
	t.buildSingularRow(f, acc)
	return f
}

// buildSingularRow wires the scalar, string, bytes and optional closures.
func (t *DType) buildSingularRow(f *Field, acc func(any) *slot) {
	switch f.Kind {
	case KindInt32:
		f.GetI32 = func(msg any) int32 { return int32(acc(msg).u) }
		f.SetI32 = func(msg any, v int32) { s := acc(msg); s.present, s.u = true, uint64(int64(v)) }
		f.GetOptI32 = func(msg any) *int32 { return optI32(acc(msg)) }
		f.SetOptI32 = func(msg any, v int32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(int64(v)), &v }
	case KindInt64:
		f.GetI64 = func(msg any) int64 { return int64(acc(msg).u) }
		f.SetI64 = func(msg any, v int64) { s := acc(msg); s.present, s.u = true, uint64(v) }
		f.GetOptI64 = func(msg any) *int64 { return optI64(acc(msg)) }
		f.SetOptI64 = func(msg any, v int64) { s := acc(msg); s.present, s.u, s.opt = true, uint64(v), &v }
	case KindUint32:
		f.GetU32 = func(msg any) uint32 { return uint32(acc(msg).u) }
		f.SetU32 = func(msg any, v uint32) { s := acc(msg); s.present, s.u = true, uint64(v) }
		f.GetOptU32 = func(msg any) *uint32 { return optU32(acc(msg)) }
		f.SetOptU32 = func(msg any, v uint32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(v), &v }
	case KindUint64:
		f.GetU64 = func(msg any) uint64 { return acc(msg).u }
		f.SetU64 = func(msg any, v uint64) { s := acc(msg); s.present, s.u = true, v }
		f.GetOptU64 = func(msg any) *uint64 { return optU64(acc(msg)) }
		f.SetOptU64 = func(msg any, v uint64) { s := acc(msg); s.present, s.u, s.opt = true, v, &v }
	case KindSint32:
		f.GetS32 = func(msg any) int32 { return int32(acc(msg).u) }
		f.SetS32 = func(msg any, v int32) { s := acc(msg); s.present, s.u = true, uint64(int64(v)) }
		f.GetOptS32 = func(msg any) *int32 { return optI32(acc(msg)) }
		f.SetOptS32 = func(msg any, v int32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(int64(v)), &v }
	case KindSint64:
		f.GetS64 = func(msg any) int64 { return int64(acc(msg).u) }
		f.SetS64 = func(msg any, v int64) { s := acc(msg); s.present, s.u = true, uint64(v) }
		f.GetOptS64 = func(msg any) *int64 { return optI64(acc(msg)) }
		f.SetOptS64 = func(msg any, v int64) { s := acc(msg); s.present, s.u, s.opt = true, uint64(v), &v }
	case KindFixed32:
		f.GetFx32 = func(msg any) uint32 { return uint32(acc(msg).u) }
		f.SetFx32 = func(msg any, v uint32) { s := acc(msg); s.present, s.u = true, uint64(v) }
		f.GetOptFx32 = func(msg any) *uint32 { return optU32(acc(msg)) }
		f.SetOptFx32 = func(msg any, v uint32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(v), &v }
	case KindFixed64:
		f.GetFx64 = func(msg any) uint64 { return acc(msg).u }
		f.SetFx64 = func(msg any, v uint64) { s := acc(msg); s.present, s.u = true, v }
		f.GetOptFx64 = func(msg any) *uint64 { return optU64(acc(msg)) }
		f.SetOptFx64 = func(msg any, v uint64) { s := acc(msg); s.present, s.u, s.opt = true, v, &v }
	case KindSfixed32:
		f.GetSfx32 = func(msg any) int32 { return int32(acc(msg).u) }
		f.SetSfx32 = func(msg any, v int32) { s := acc(msg); s.present, s.u = true, uint64(int64(v)) }
		f.GetOptSfx32 = func(msg any) *int32 { return optI32(acc(msg)) }
		f.SetOptSfx32 = func(msg any, v int32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(int64(v)), &v }
	case KindSfixed64:
		f.GetSfx64 = func(msg any) int64 { return int64(acc(msg).u) }
		f.SetSfx64 = func(msg any, v int64) { s := acc(msg); s.present, s.u = true, uint64(v) }
		f.GetOptSfx64 = func(msg any) *int64 { return optI64(acc(msg)) }
		f.SetOptSfx64 = func(msg any, v int64) { s := acc(msg); s.present, s.u, s.opt = true, uint64(v), &v }
	case KindFloat:
		f.GetF32 = func(msg any) float32 { return math.Float32frombits(uint32(acc(msg).u)) }
		f.SetF32 = func(msg any, v float32) {
			s := acc(msg)
			s.present, s.u = true, uint64(math.Float32bits(v))
		}
		f.GetOptF32 = func(msg any) *float32 { return optF32(acc(msg)) }
		f.SetOptF32 = func(msg any, v float32) {
			s := acc(msg)
			s.present, s.u, s.opt = true, uint64(math.Float32bits(v)), &v
		}
	case KindDouble:
		f.GetF64 = func(msg any) float64 { return math.Float64frombits(acc(msg).u) }
		f.SetF64 = func(msg any, v float64) {
			s := acc(msg)
			s.present, s.u = true, math.Float64bits(v)
		}
		f.GetOptF64 = func(msg any) *float64 { return optF64(acc(msg)) }
		f.SetOptF64 = func(msg any, v float64) {
			s := acc(msg)
			s.present, s.u, s.opt = true, math.Float64bits(v), &v
		}
	case KindBool:
		f.GetBool = func(msg any) bool { return acc(msg).u != 0 }
		f.SetBool = func(msg any, v bool) { s := acc(msg); s.present, s.u = true, boolBits(v) }
		f.GetOptBool = func(msg any) *bool { return optBool(acc(msg)) }
		f.SetOptBool = func(msg any, v bool) { s := acc(msg); s.present, s.u, s.opt = true, boolBits(v), &v }
	case KindEnum:
		f.GetEnum = func(msg any) int32 { return int32(acc(msg).u) }
		f.SetEnum = func(msg any, v int32) { s := acc(msg); s.present, s.u = true, uint64(int64(v)) }
		f.GetOptEnum = func(msg any) *int32 { return optI32(acc(msg)) }
		f.SetOptEnum = func(msg any, v int32) { s := acc(msg); s.present, s.u, s.opt = true, uint64(int64(v)), &v }
	case KindString:
		f.GetStr = func(msg any) string { return acc(msg).str }
		f.SetStr = func(msg any, v string) { s := acc(msg); s.present, s.str = true, v }
		f.GetOptStr = func(msg any) *string { return optStr(acc(msg)) }
		f.SetOptStr = func(msg any, v string) { s := acc(msg); s.present, s.str, s.opt = true, v, &v }
	case KindBytes:
		f.GetBytes = func(msg any) []byte { return acc(msg).p }
		f.SetBytes = func(msg any, v []byte) { s := acc(msg); s.present, s.p = true, v }
		f.GetOptBytes = func(msg any) *[]byte { return optBytes(acc(msg)) }
		f.SetOptBytes = func(msg any, v []byte) { s := acc(msg); s.present, s.p, s.opt = true, v, &v }
	}
}

// buildRepeatedRow wires the append/get slice closures.
func (t *DType) buildRepeatedRow(f *Field, fp *FieldPlan, acc func(any) *slot) {
	switch f.Kind {
	case KindInt32:
		f.GetRepI32 = func(msg any) []int32 { return repOf[int32](acc(msg)) }
		f.AppendI32 = func(msg any, v int32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindInt64:
		f.GetRepI64 = func(msg any) []int64 { return repOf[int64](acc(msg)) }
		f.AppendI64 = func(msg any, v int64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindUint32:
		f.GetRepU32 = func(msg any) []uint32 { return repOf[uint32](acc(msg)) }
		f.AppendU32 = func(msg any, v uint32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindUint64:
		f.GetRepU64 = func(msg any) []uint64 { return repOf[uint64](acc(msg)) }
		f.AppendU64 = func(msg any, v uint64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindSint32:
		f.GetRepS32 = func(msg any) []int32 { return repOf[int32](acc(msg)) }
		f.AppendS32 = func(msg any, v int32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindSint64:
		f.GetRepS64 = func(msg any) []int64 { return repOf[int64](acc(msg)) }
		f.AppendS64 = func(msg any, v int64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindFixed32:
		f.GetRepFx32 = func(msg any) []uint32 { return repOf[uint32](acc(msg)) }
		f.AppendFx32 = func(msg any, v uint32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindFixed64:
		f.GetRepFx64 = func(msg any) []uint64 { return repOf[uint64](acc(msg)) }
		f.AppendFx64 = func(msg any, v uint64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindSfixed32:
		f.GetRepSfx32 = func(msg any) []int32 { return repOf[int32](acc(msg)) }
		f.AppendSfx32 = func(msg any, v int32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindSfixed64:
		f.GetRepSfx64 = func(msg any) []int64 { return repOf[int64](acc(msg)) }
		f.AppendSfx64 = func(msg any, v int64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindFloat:
		f.GetRepF32 = func(msg any) []float32 { return repOf[float32](acc(msg)) }
		f.AppendF32 = func(msg any, v float32) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindDouble:
		f.GetRepF64 = func(msg any) []float64 { return repOf[float64](acc(msg)) }
		f.AppendF64 = func(msg any, v float64) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindBool:
		f.GetRepBool = func(msg any) []bool { return repOf[bool](acc(msg)) }
		f.AppendBool = func(msg any, v bool) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindString:
		f.GetRepStr = func(msg any) []string { return repOf[string](acc(msg)) }
		f.AppendStr = func(msg any, v string) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindBytes:
		f.GetRepBytes = func(msg any) [][]byte { return repOf[[]byte](acc(msg)) }
		f.AppendBytes = func(msg any, v []byte) { s := acc(msg); s.present = true; appendRep(s, v) }
	case KindEnum:
		f.GetRepEnum = func(msg any) []int32 { return repOf[int32](acc(msg)) }
		f.AppendEnum = func(msg any, v int32) { s := acc(msg); s.present = true; appendRep(s, v) }
	}
}

// pointer helpers: an explicit-presence scalar's getter hands back the pointer
// the setter stored, so the engine's nil check is the presence check.
func optI32(s *slot) *int32 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*int32)
}

func optI64(s *slot) *int64 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*int64)
}

func optU32(s *slot) *uint32 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*uint32)
}

func optU64(s *slot) *uint64 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*uint64)
}

func optF32(s *slot) *float32 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*float32)
}

func optF64(s *slot) *float64 {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*float64)
}

func optBool(s *slot) *bool {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*bool)
}

func optStr(s *slot) *string {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*string)
}

func optBytes(s *slot) *[]byte {
	if s.opt == nil {
		return nil
	}
	return s.opt.(*[]byte)
}

func boolBits(v bool) uint64 {
	if v {
		return 1
	}
	return 0
}

// appendRepeatedRep concatenates two repeated runs without knowing the typed
// slice behind them; every repeated append funnels through this so split
// occurrences merge element by element instead of replacing the run.
func appendRepeatedRep(dst, src any) any {
	if dst == nil {
		return src
	}
	if src == nil {
		return dst
	}
	switch in := src.(type) {
	case []int32:
		out := append([]int32(nil), dst.([]int32)...)
		return append(out, in...)
	case []int64:
		out := append([]int64(nil), dst.([]int64)...)
		return append(out, in...)
	case []uint32:
		out := append([]uint32(nil), dst.([]uint32)...)
		return append(out, in...)
	case []uint64:
		out := append([]uint64(nil), dst.([]uint64)...)
		return append(out, in...)
	case []float32:
		out := append([]float32(nil), dst.([]float32)...)
		return append(out, in...)
	case []float64:
		out := append([]float64(nil), dst.([]float64)...)
		return append(out, in...)
	case []bool:
		out := append([]bool(nil), dst.([]bool)...)
		return append(out, in...)
	case []string:
		out := append([]string(nil), dst.([]string)...)
		return append(out, in...)
	case [][]byte:
		out := append([][]byte(nil), dst.([][]byte)...)
		return append(out, in...)
	case []*DMsg:
		out := append([]*DMsg(nil), dst.([]*DMsg)...)
		return append(out, in...)
	}
	return src
}
