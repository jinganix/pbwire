// Extension field codec: storage lives on DMsg.ext (see dmsg.go), one slot
// per set extension in field-number order. Binary encode and size reuse the
// slot shapes, text parse and print live in text.go; this file only frames
// extension values on the wire, mirroring the encodeTable singular and
// repeated shapes for every kind.
package runtime

import (
	"math"

	"google.golang.org/protobuf/types/descriptorpb"
)

// encodeSubmessage encodes one nested message body: plan fields via
// encodeTable plus any extensions the dynamic tree holds. Generated
// messages never carry ext, so the extra step is a type-asserted no-op
// for them. Callers pass the depth the child encodes at (parent+1),
// matching the two-step order Encode uses at the top level.
func encodeSubmessage(e *Encoder, sub any, tbl *Table, depth int, deterministic bool) error {
	if err := encodeTable(e, sub, tbl, depth, deterministic); err != nil {
		return err
	}
	if dm, ok := sub.(*DMsg); ok && dm != nil {
		return encodeExtTable(e, dm, depth, deterministic)
	}
	return nil
}

// encodeExtTable writes every present extension after the plan fields, in
// field-number order (findExt keeps the slice sorted). It runs after
// encodeTable in Encode and EncodeDeterministic alike, and after every
// nested encodeTable via encodeSubmessage.
func encodeExtTable(e *Encoder, m *DMsg, depth int, deterministic bool) error {
	if len(m.ext) == 0 {
		return nil
	}
	if err := CheckDepth(depth); err != nil {
		return err
	}
	for i := range m.ext {
		fp := extField(&m.ext[i])
		s := &m.ext[i].s
		if !s.present {
			continue
		}
		if fp.Repeated {
			if err := encodeExtRep(e, m, fp, s, depth, deterministic); err != nil {
				return err
			}
			continue
		}
		if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE || fp.Group {
			if err := encodeExtMsg(e, m, fp, s.m, depth, deterministic); err != nil {
				return err
			}
			continue
		}
		e.Tag(fp.Number, fp.Wire)
		encodeExtScalar(e, fp, s.u, s.str, s.p)
	}
	return nil
}

// encodeExtMsg frames one singular extension submessage, group or not. A
// present-but-nil slot encodes empty, exactly like fieldSize sizes it.
func encodeExtMsg(e *Encoder, m *DMsg, fp *FieldPlan, sub *DMsg, depth int, deterministic bool) error {
	if fp.Group {
		e.Tag(fp.Number, WireStartGroup)
		if sub != nil {
			if err := encodeSubmessage(e, sub, sub.typ.tableFor(), depth+1, deterministic); err != nil {
				return err
			}
		}
		e.Tag(fp.Number, WireEndGroup)
		return nil
	}
	mark := e.Begin(fp.Number)
	if sub != nil {
		if err := encodeSubmessage(e, sub, sub.typ.tableFor(), depth+1, deterministic); err != nil {
			return err
		}
	}
	e.End(mark)
	return nil
}

// encodeExtRep writes one repeated extension: messages recurse per
// element, packable numerics share one length prefix when the row packs,
// everything else repeats the tag like the encodeRepeated engine.
func encodeExtRep(e *Encoder, m *DMsg, fp *FieldPlan, s *slot, depth int, deterministic bool) error {
	if fp.Group {
		for _, sub := range s.rep.([]*DMsg) {
			e.Tag(fp.Number, WireStartGroup)
			if sub != nil {
				if err := encodeSubmessage(e, sub, sub.typ.tableFor(), depth+1, deterministic); err != nil {
					return err
				}
			}
			e.Tag(fp.Number, WireEndGroup)
		}
		return nil
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_MESSAGE {
		for _, sub := range s.rep.([]*DMsg) {
			mark := e.Begin(fp.Number)
			if sub != nil {
				if err := encodeSubmessage(e, sub, sub.typ.tableFor(), depth+1, deterministic); err != nil {
					return err
				}
			}
			e.End(mark)
		}
		return nil
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_STRING {
		for _, v := range s.rep.([]string) {
			e.Tag(fp.Number, WireLen)
			e.Text(v)
		}
		return nil
	}
	if fp.Proto == descriptorpb.FieldDescriptorProto_TYPE_BYTES {
		for _, v := range s.rep.([][]byte) {
			e.Tag(fp.Number, WireLen)
			e.Data(v)
		}
		return nil
	}
	if fp.Packed {
		mark := e.Begin(fp.Number)
		encodeExtPacked(e, fp, s)
		e.End(mark)
		return nil
	}
	switch run := s.rep.(type) {
	case []int32:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			encodeExtInt(e, fp, uint64(int64(v)))
		}
	case []int64:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			encodeExtInt(e, fp, uint64(v))
		}
	case []uint32:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			encodeExtInt(e, fp, uint64(v))
		}
	case []uint64:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			encodeExtInt(e, fp, v)
		}
	case []float32:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			e.Fixed32(math.Float32bits(v))
		}
	case []float64:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			e.Fixed64(math.Float64bits(v))
		}
	case []bool:
		for _, v := range run {
			e.Tag(fp.Number, fp.Wire)
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
		}
	}
	return nil
}

// encodeExtPacked writes the body of one packed numeric run: raw payloads
// with no tags, like the packed arm of the encodeRepeated engine.
func encodeExtPacked(e *Encoder, fp *FieldPlan, s *slot) {
	switch run := s.rep.(type) {
	case []int32:
		for _, v := range run {
			encodeExtInt(e, fp, uint64(int64(v)))
		}
	case []int64:
		for _, v := range run {
			encodeExtInt(e, fp, uint64(v))
		}
	case []uint32:
		for _, v := range run {
			encodeExtInt(e, fp, uint64(v))
		}
	case []uint64:
		for _, v := range run {
			encodeExtInt(e, fp, v)
		}
	case []float32:
		for _, v := range run {
			e.Fixed32(math.Float32bits(v))
		}
	case []float64:
		for _, v := range run {
			e.Fixed64(math.Float64bits(v))
		}
	case []bool:
		for _, v := range run {
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
		}
	}
}

// encodeExtInt writes one integer payload with zigzag where the kind
// needs it, mirroring the encodeTable integer arms.
func encodeExtInt(e *Encoder, fp *FieldPlan, u uint64) {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_SINT32:
		e.Varint(ZigZagEncode32(int32(u)))
	case descriptorpb.FieldDescriptorProto_TYPE_SINT64:
		e.Varint(ZigZagEncode64(int64(u)))
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED32,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED32:
		e.Fixed32(uint32(u))
	case descriptorpb.FieldDescriptorProto_TYPE_FIXED64,
		descriptorpb.FieldDescriptorProto_TYPE_SFIXED64:
		e.Fixed64(u)
	default:
		e.Varint(u)
	}
}

// encodeExtScalar writes one singular scalar payload from slot parts,
// mirroring the encodeTable singular arms (u holds integer bits, enum
// numbers, float/double bits and bools; str/p hold strings and bytes).
func encodeExtScalar(e *Encoder, fp *FieldPlan, u uint64, str string, p []byte) {
	switch fp.Proto {
	case descriptorpb.FieldDescriptorProto_TYPE_STRING:
		e.Text(str)
	case descriptorpb.FieldDescriptorProto_TYPE_BYTES:
		e.Data(p)
	case descriptorpb.FieldDescriptorProto_TYPE_FLOAT:
		e.Fixed32(uint32(u))
	case descriptorpb.FieldDescriptorProto_TYPE_DOUBLE:
		e.Fixed64(u)
	case descriptorpb.FieldDescriptorProto_TYPE_BOOL:
		if u != 0 {
			e.Varint(1)
		} else {
			e.Varint(0)
		}
	default:
		encodeExtInt(e, fp, u)
	}
}
