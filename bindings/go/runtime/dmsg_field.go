// Public field access on DMsg. Every method resolves a field name through the
// message's plan, so a typo fails loudly instead of writing nowhere.
//
// Values are semantic, not raw bits: a sint32 set to -7 reads back -7, and the
// engine does the zigzag on the wire. Repeated setters append, singular setters
// replace, and a oneof member clears its siblings.
package runtime

import (
	"fmt"
	"math"

	"google.golang.org/protobuf/types/descriptorpb"
)

// find resolves the plan field and its slot ordinal.
func (m *DMsg) find(name string) (*FieldPlan, int, error) {
	if m == nil {
		return nil, 0, fmt.Errorf("codec: nil message")
	}
	fp, ok := m.typ.plan.ByName(name)
	if !ok {
		return nil, 0, fmt.Errorf("codec: %s has no field %q", m.typ.plan.Full, name)
	}
	for i, f := range m.typ.plan.Fields {
		if f == fp {
			return fp, i, nil
		}
	}
	return nil, 0, fmt.Errorf("codec: field %q missing from %s", name, m.typ.plan.Full)
}

func protoErr(fp *FieldPlan, want string) error {
	return fmt.Errorf("codec: field %s is %s, not %s", fp.Name, fp.Proto, want)
}

// mark writes presence and, for a oneof member, the unwrapped value the engine
// encodes, clearing the siblings so only one member is set. An explicit-presence
// scalar also stores the pointer its getter hands the engine.
func (m *DMsg) mark(i int, v any) {
	s := &m.slots[i]
	s.present = true
	fp := m.typ.plan.Fields[i]
	if fp.Presence == PresenceOneof {
		mergeOneof(m, i, v)
		return
	}
	if explicitPresence(fp) {
		storeOpt(s, v)
	}
}

// storeOneof writes a set oneof member's value into the slot's typed storage
// too, so the field getters read it like any other field.
func storeOneof(s *slot, v any) {
	switch t := v.(type) {
	case int32:
		s.u = uint64(int64(t))
	case int64:
		s.u = uint64(t)
	case uint32:
		s.u = uint64(t)
	case uint64:
		s.u = t
	case float32:
		s.u = uint64(math.Float32bits(t))
	case float64:
		s.u = math.Float64bits(t)
	case bool:
		s.u = boolBits(t)
	case string:
		s.str = t
	case []byte:
		s.p = t
	case *DMsg:
		s.m = t
	}
}

// storeOpt keeps one pointer for an explicit-presence scalar, so the engine's
// nil check reads presence without a per-encode allocation.
func storeOpt(s *slot, v any) {
	switch t := v.(type) {
	case int32:
		s.opt = &t
	case int64:
		s.opt = &t
	case uint32:
		s.opt = &t
	case uint64:
		s.opt = &t
	case float32:
		s.opt = &t
	case float64:
		s.opt = &t
	case bool:
		s.opt = &t
	case string:
		s.opt = &t
	case []byte:
		s.opt = &t
	}
}

// SetInt stores a signed integer, enum, or bool field. Repeated fields append.
func (m *DMsg) SetInt(name string, v int64) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.IsMap {
		return protoErr(fp, "a scalar")
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return m.putInt(fp, i, int32(v))
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return m.putInt(fp, i, v)
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return m.putInt(fp, i, uint32(v))
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		return m.putInt(fp, i, uint64(v))
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return m.putInt(fp, i, v != 0)
	}
	return protoErr(fp, "an integer")
}

// SetUint stores an unsigned integer field. Repeated fields append.
func (m *DMsg) SetUint(name string, v uint64) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return m.putInt(fp, i, uint32(v))
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		return m.putInt(fp, i, v)
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return m.putInt(fp, i, int32(v))
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return m.putInt(fp, i, int64(v))
	}
	return protoErr(fp, "an integer")
}

// putInt stores one integer-family value, typed for the slot and the engine.
func (m *DMsg) putInt(fp *FieldPlan, i int, v any) error {
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		switch t := v.(type) {
		case int32:
			appendRep(s, t)
		case int64:
			appendRep(s, t)
		case uint32:
			appendRep(s, t)
		case uint64:
			appendRep(s, t)
		case bool:
			appendRep(s, t)
		}
		return nil
	}
	s := &m.slots[i]
	switch t := v.(type) {
	case int32:
		s.u = uint64(int64(t))
	case int64:
		s.u = uint64(t)
	case uint32:
		s.u = uint64(t)
	case uint64:
		s.u = t
	case bool:
		s.u = boolBits(t)
	}
	m.mark(i, v)
	return nil
}

// SetBool stores a bool field. Repeated fields append.
func (m *DMsg) SetBool(name string, v bool) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_BOOL {
		return protoErr(fp, "a bool")
	}
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, v)
		return nil
	}
	m.slots[i].u = boolBits(v)
	m.mark(i, v)
	return nil
}

// SetFloat stores a float field. Repeated fields append.
func (m *DMsg) SetFloat(name string, v float32) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_FLOAT {
		return protoErr(fp, "a float")
	}
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, v)
		return nil
	}
	m.slots[i].u = uint64(math.Float32bits(v))
	m.mark(i, v)
	return nil
}

// SetDouble stores a double field. Repeated fields append.
func (m *DMsg) SetDouble(name string, v float64) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_DOUBLE {
		return protoErr(fp, "a double")
	}
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, v)
		return nil
	}
	m.slots[i].u = math.Float64bits(v)
	m.mark(i, v)
	return nil
}

// SetString stores a string field. Repeated fields append.
func (m *DMsg) SetString(name string, v string) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return protoErr(fp, "a string")
	}
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, v)
		return nil
	}
	m.slots[i].str = v
	m.mark(i, v)
	return nil
}

// SetBytes stores a bytes field. The payload is copied. Repeated fields append.
func (m *DMsg) SetBytes(name string, v []byte) error {
	fp, i, err := m.find(name)
	if err != nil {
		return err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_BYTES {
		return protoErr(fp, "bytes")
	}
	cp := append([]byte(nil), v...)
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, cp)
		return nil
	}
	m.slots[i].p = cp
	m.mark(i, cp)
	return nil
}

// Has reports whether the field carries a value.
func (m *DMsg) Has(name string) (bool, error) {
	_, i, err := m.find(name)
	if err != nil {
		return false, err
	}
	return m.slots[i].present, nil
}

// GetInt reads an integer, enum or bool field as raw bits, sign-extended to
// its declared width. ok is false when the field is absent.
func (m *DMsg) GetInt(name string) (int64, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return 0, false, err
	}
	s := &m.slots[i]
	if !s.present {
		return 0, false, nil
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return int64(int32(s.u)), true, nil
	case descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return int64(s.u), true, nil
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32:
		return int64(uint32(s.u)), true, nil
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64:
		return int64(s.u), true, nil
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		return lastBit(s.u), true, nil
	}
	return 0, false, protoErr(fp, "an integer")
}

// GetUint reads an unsigned integer field.
func (m *DMsg) GetUint(name string) (uint64, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return 0, false, err
	}
	s := &m.slots[i]
	if !s.present {
		return 0, false, nil
	}
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_UINT32,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_INT32,
		descriptorpb.FieldDescriptorProto_TYPE_SINT32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_ENUM:
		return uint64(uint32(s.u)), true, nil
	case descriptorpb.FieldDescriptorProto_TYPE_UINT64,
		descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_INT64,
		descriptorpb.FieldDescriptorProto_TYPE_SINT64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		return s.u, true, nil
	}
	return 0, false, protoErr(fp, "an integer")
}

// GetBool reads a bool field.
func (m *DMsg) GetBool(name string) (bool, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return false, false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_BOOL {
		return false, false, protoErr(fp, "a bool")
	}
	s := &m.slots[i]
	return s.u != 0, s.present, nil
}

// GetFloat reads a float field.
func (m *DMsg) GetFloat(name string) (float32, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return 0, false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_FLOAT {
		return 0, false, protoErr(fp, "a float")
	}
	s := &m.slots[i]
	return math.Float32frombits(uint32(s.u)), s.present, nil
}

// GetDouble reads a double field.
func (m *DMsg) GetDouble(name string) (float64, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return 0, false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_DOUBLE {
		return 0, false, protoErr(fp, "a double")
	}
	s := &m.slots[i]
	return math.Float64frombits(s.u), s.present, nil
}

// GetString reads a string field.
func (m *DMsg) GetString(name string) (string, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return "", false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_STRING {
		return "", false, protoErr(fp, "a string")
	}
	s := &m.slots[i]
	return s.str, s.present, nil
}

// GetBytes reads a bytes field. The slice is owned by the message.
func (m *DMsg) GetBytes(name string) ([]byte, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return nil, false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_BYTES {
		return nil, false, protoErr(fp, "bytes")
	}
	s := &m.slots[i]
	return s.p, s.present, nil
}

func lastBit(u uint64) int64 {
	if u != 0 {
		return 1
	}
	return 0
}

// sliceOf returns a repeated field's typed run. Handing back the run itself
// keeps reads allocation-free; a caller that mutates it mutates the message.
func sliceOf[T any](m *DMsg, name string, want descriptorpb.FieldDescriptorProto_Type) ([]T, error) {
	fp, i, err := m.find(name)
	if err != nil {
		return nil, err
	}
	if !fp.Repeated {
		return nil, protoErr(fp, "repeated")
	}
	if fp.Proto != want {
		return nil, protoErr(fp, want.String())
	}
	s := &m.slots[i]
	if s.rep == nil {
		return nil, nil
	}
	return s.rep.([]T), nil
}

// GetIntSlice reads a repeated integer, enum or bool field.
func (m *DMsg) GetIntSlice(name string) ([]int64, error) {
	fp, i, err := m.find(name)
	if err != nil {
		return nil, err
	}
	if !fp.Repeated {
		return nil, protoErr(fp, "repeated")
	}
	s := &m.slots[i]
	if s.rep == nil {
		return nil, nil
	}
	switch t := s.rep.(type) {
	case []int32:
		out := make([]int64, len(t))
		for k, v := range t {
			out[k] = int64(v)
		}
		return out, nil
	case []int64:
		out := make([]int64, len(t))
		copy(out, t)
		return out, nil
	case []uint32:
		out := make([]int64, len(t))
		for k, v := range t {
			out[k] = int64(v)
		}
		return out, nil
	case []uint64:
		out := make([]int64, len(t))
		for k, v := range t {
			out[k] = int64(v)
		}
		return out, nil
	case []bool:
		out := make([]int64, len(t))
		for k, v := range t {
			if v {
				out[k] = 1
			}
		}
		return out, nil
	}
	return nil, protoErr(fp, "integers")
}

// GetUintSlice reads a repeated unsigned field.
func (m *DMsg) GetUintSlice(name string) ([]uint64, error) {
	fp, i, err := m.find(name)
	if err != nil {
		return nil, err
	}
	if !fp.Repeated {
		return nil, protoErr(fp, "repeated")
	}
	s := &m.slots[i]
	switch t := s.rep.(type) {
	case nil:
		return nil, nil
	case []uint32:
		out := make([]uint64, len(t))
		for k, v := range t {
			out[k] = uint64(v)
		}
		return out, nil
	case []uint64:
		out := make([]uint64, len(t))
		copy(out, t)
		return out, nil
	case []int32:
		out := make([]uint64, len(t))
		for k, v := range t {
			out[k] = uint64(uint32(v))
		}
		return out, nil
	case []int64:
		out := make([]uint64, len(t))
		for k, v := range t {
			out[k] = uint64(v)
		}
		return out, nil
	}
	return nil, protoErr(fp, "integers")
}

// GetBoolSlice reads a repeated bool field.
func (m *DMsg) GetBoolSlice(name string) ([]bool, error) {
	run, err := sliceOf[bool](m, name, descriptorpb.FieldDescriptorProto_TYPE_BOOL)
	return run, err
}

// GetFloatSlice reads a repeated float field.
func (m *DMsg) GetFloatSlice(name string) ([]float32, error) {
	run, err := sliceOf[float32](m, name, descriptorpb.FieldDescriptorProto_TYPE_FLOAT)
	return run, err
}

// GetDoubleSlice reads a repeated double field.
func (m *DMsg) GetDoubleSlice(name string) ([]float64, error) {
	run, err := sliceOf[float64](m, name, descriptorpb.FieldDescriptorProto_TYPE_DOUBLE)
	return run, err
}

// GetStringSlice reads a repeated string field.
func (m *DMsg) GetStringSlice(name string) ([]string, error) {
	run, err := sliceOf[string](m, name, descriptorpb.FieldDescriptorProto_TYPE_STRING)
	return run, err
}

// GetBytesSlice reads a repeated bytes field.
func (m *DMsg) GetBytesSlice(name string) ([][]byte, error) {
	run, err := sliceOf[[]byte](m, name, descriptorpb.FieldDescriptorProto_TYPE_BYTES)
	return run, err
}

// NewMessage creates (or replaces) a singular message field and returns it, or
// appends a new element for a repeated one. The child shares the parent's schema.
func (m *DMsg) NewMessage(name string) (*DMsg, error) {
	fp, i, err := m.find(name)
	if err != nil {
		return nil, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
		return nil, protoErr(fp, "a message")
	}
	sub, err := m.typ.sub(i)
	if err != nil {
		return nil, err
	}
	child := sub.New()
	if fp.Repeated {
		s := &m.slots[i]
		s.present = true
		appendRep(s, child)
		return child, nil
	}
	if fp.Presence == PresenceOneof {
		m.slots[i].m = child
		m.mark(i, child)
		return child, nil
	}
	m.slots[i].m = child
	m.slots[i].present = true
	return child, nil
}

// GetMessage reads a singular message field as a view into this tree.
func (m *DMsg) GetMessage(name string) (*DMsg, bool, error) {
	fp, i, err := m.find(name)
	if err != nil || fp.Repeated {
		return nil, false, err
	}
	if fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group {
		return nil, false, protoErr(fp, "a message")
	}
	s := &m.slots[i]
	return s.m, s.m != nil, nil
}

// GetMessageSlice reads a repeated message field as views into this tree.
func (m *DMsg) GetMessageSlice(name string) ([]*DMsg, error) {
	fp, i, err := m.find(name)
	if err != nil {
		return nil, err
	}
	if !fp.Repeated || (fp.Proto != descriptorpb.FieldDescriptorProto_TYPE_MESSAGE && !fp.Group) {
		return nil, protoErr(fp, "a repeated message")
	}
	return repOf[*DMsg](&m.slots[i]), nil
}
