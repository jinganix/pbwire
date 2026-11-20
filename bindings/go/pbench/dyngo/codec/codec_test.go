// Usability proof: a hand-written straight-line model for
// structtest.Scalars3/Inner3 on top of the codec API, in the exact shape the
// codegen emitter will later generate (per-field Tag/typed appends on
// encode; a Next/Switch/Skip loop on decode). Field numbers below are
// schema-derived: the plan test pins them against the descriptor set, and
// the tier gate below proves them byte-equal to protobuf-go.
package codec_test

import (
	"bytes"
	"math"
	"os"
	"reflect"
	"sort"
	"testing"
	"unsafe"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
	"pbwire/pbench/dyngo/codec"
	"pbwire/tests/gen/matrix3"
	"pbwire/tests/gen_oracle/matrix3pb"
)

// ---------------------------------------------------------------------------
// Straight-line model, encode side.
// ---------------------------------------------------------------------------

func marshalInner(e *codec.Encoder, m *matrix3.Inner3, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	if m.Name != "" {
		e.Tag(1, codec.WireLen)
		e.Text(m.Name)
	}
	if m.Value != 0 {
		e.Tag(2, codec.WireVarint)
		e.Varint(uint64(int64(m.Value)))
	}
	return nil
}

func marshalScalars(e *codec.Encoder, m *matrix3.Scalars3, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	if m.I32 != 0 {
		e.Tag(1, codec.WireVarint)
		e.Varint(uint64(int64(m.I32)))
	}
	if m.I64 != 0 {
		e.Tag(2, codec.WireVarint)
		e.Varint(uint64(m.I64))
	}
	if m.U32 != 0 {
		e.Tag(3, codec.WireVarint)
		e.Varint(uint64(m.U32))
	}
	if m.U64 != 0 {
		e.Tag(4, codec.WireVarint)
		e.Varint(m.U64)
	}
	if m.S32 != 0 {
		e.Tag(5, codec.WireVarint)
		e.Varint(codec.ZigZagEncode32(m.S32))
	}
	if m.S64 != 0 {
		e.Tag(6, codec.WireVarint)
		e.Varint(codec.ZigZagEncode64(m.S64))
	}
	if m.Fx32 != 0 {
		e.Tag(7, codec.WireFixed32)
		e.Fixed32(m.Fx32)
	}
	if m.Fx64 != 0 {
		e.Tag(8, codec.WireFixed64)
		e.Fixed64(m.Fx64)
	}
	if m.Sfx32 != 0 {
		e.Tag(9, codec.WireFixed32)
		e.Fixed32(uint32(m.Sfx32))
	}
	if m.Sfx64 != 0 {
		e.Tag(10, codec.WireFixed64)
		e.Fixed64(uint64(m.Sfx64))
	}
	if b := math.Float32bits(m.F32); b != 0 {
		e.Tag(11, codec.WireFixed32)
		e.Fixed32(b)
	}
	if b := math.Float64bits(m.F64); b != 0 {
		e.Tag(12, codec.WireFixed64)
		e.Fixed64(b)
	}
	if m.Flag {
		e.Tag(13, codec.WireVarint)
		e.Varint(1)
	}
	if m.Str != "" {
		e.Tag(14, codec.WireLen)
		e.Text(m.Str)
	}
	if m.Color != 0 {
		e.Tag(15, codec.WireVarint)
		e.Varint(uint64(int64(m.Color)))
	}
	if m.Inner != nil {
		mark := e.Begin(16)
		if err := marshalInner(e, m.Inner, depth+1); err != nil {
			return err
		}
		e.End(mark)
	}
	if len(m.Nums) != 0 {
		mark := e.Begin(17)
		for _, v := range m.Nums {
			e.Varint(uint64(int64(v)))
		}
		e.End(mark)
	}
	for _, s := range m.Tags {
		e.Tag(18, codec.WireLen)
		e.Text(s)
	}
	for _, sub := range m.Inners {
		mark := e.Begin(19)
		if err := marshalInner(e, sub, depth+1); err != nil {
			return err
		}
		e.End(mark)
	}
	if m.OptI32 != nil {
		e.Tag(20, codec.WireVarint)
		e.Varint(uint64(int64(*m.OptI32)))
	}
	if m.OptStr != nil {
		e.Tag(21, codec.WireLen)
		e.Text(*m.OptStr)
	}
	if len(m.Flags) != 0 {
		mark := e.Begin(22)
		for _, v := range m.Flags {
			if v {
				e.Varint(1)
			} else {
				e.Varint(0)
			}
		}
		e.End(mark)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Straight-line model, decode side. One Arena per call owns every decoded
// string; presence writes pointers/wrappers, exactly like generated code.
// ---------------------------------------------------------------------------

func unmarshalInner(d *codec.Decoder, a *codec.Arena, m *matrix3.Inner3, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		switch num {
		case 1:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Name = a.AddString(raw)
		case 2:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.Value = int32(v)
		default:
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

func unmarshalPackedInt32(raw []byte, dst *[]int32) error {
	n, err := codec.CountVarints(raw)
	if err != nil {
		return err
	}
	sub := codec.Decoder{Buf: raw}
	if cap(*dst)-len(*dst) < n {
		next := make([]int32, len(*dst), len(*dst)+n)
		copy(next, *dst)
		*dst = next
	}
	for !sub.Done() {
		v, err := sub.Varint()
		if err != nil {
			return err
		}
		*dst = append(*dst, int32(v))
	}
	return nil
}

func unmarshalPackedBool(raw []byte, dst *[]bool) error {
	n, err := codec.CountVarints(raw)
	if err != nil {
		return err
	}
	sub := codec.Decoder{Buf: raw}
	if cap(*dst)-len(*dst) < n {
		next := make([]bool, len(*dst), len(*dst)+n)
		copy(next, *dst)
		*dst = next
	}
	for !sub.Done() {
		v, err := sub.Varint()
		if err != nil {
			return err
		}
		*dst = append(*dst, v != 0)
	}
	return nil
}

func unmarshalScalars(d *codec.Decoder, a *codec.Arena, m *matrix3.Scalars3, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		switch num {
		case 1:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.I32 = int32(v)
		case 2:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.I64 = int64(v)
		case 3:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.U32 = uint32(v)
		case 4:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.U64 = v
		case 5:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.S32 = codec.ZigZagDecode32(v)
		case 6:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.S64 = codec.ZigZagDecode64(v)
		case 7:
			if wt != codec.WireFixed32 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			m.Fx32 = v
		case 8:
			if wt != codec.WireFixed64 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			m.Fx64 = v
		case 9:
			if wt != codec.WireFixed32 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			m.Sfx32 = int32(v)
		case 10:
			if wt != codec.WireFixed64 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			m.Sfx64 = int64(v)
		case 11:
			if wt != codec.WireFixed32 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed32()
			if err != nil {
				return err
			}
			m.F32 = math.Float32frombits(v)
		case 12:
			if wt != codec.WireFixed64 {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Fixed64()
			if err != nil {
				return err
			}
			m.F64 = math.Float64frombits(v)
		case 13:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.Flag = v != 0
		case 14:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Str = a.AddString(raw)
		case 15:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.Color = matrix3.Color3(int32(v)) // unknown values preserved
		case 16:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			if m.Inner == nil {
				m.Inner = new(matrix3.Inner3)
			}
			sub := codec.Decoder{Buf: raw}
			if err := unmarshalInner(&sub, a, m.Inner, depth+1); err != nil {
				return err
			}
		case 17:
			switch wt {
			case codec.WireVarint:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				m.Nums = append(m.Nums, int32(v))
			case codec.WireLen:
				raw, err := d.Bytes()
				if err != nil {
					return err
				}
				if err := unmarshalPackedInt32(raw, &m.Nums); err != nil {
					return err
				}
			default:
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
			}
		case 18:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Tags = append(m.Tags, a.AddString(raw))
		case 19:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			sub := new(matrix3.Inner3)
			m.Inners = append(m.Inners, sub)
			sd := codec.Decoder{Buf: raw}
			if err := unmarshalInner(&sd, a, sub, depth+1); err != nil {
				return err
			}
		case 20:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			tmp := int32(v)
			m.OptI32 = &tmp
		case 21:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			tmp := a.AddString(raw)
			m.OptStr = &tmp
		case 22:
			switch wt {
			case codec.WireVarint:
				v, err := d.Varint()
				if err != nil {
					return err
				}
				m.Flags = append(m.Flags, v != 0)
			case codec.WireLen:
				raw, err := d.Bytes()
				if err != nil {
					return err
				}
				if err := unmarshalPackedBool(raw, &m.Flags); err != nil {
					return err
				}
			default:
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
			}
		default:
			// Unknown fields (including extensions) are dropped, as on
			// the dynamic path.
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

// Top-level helpers: fresh output, one arena per call.
func encodeScalars(m *matrix3.Scalars3) ([]byte, error) {
	e := codec.Encoder{Buf: make([]byte, 0, 256)}
	if err := marshalScalars(&e, m, 0); err != nil {
		return nil, err
	}
	return e.Result(), nil
}

func decodeScalars(wire []byte) (*matrix3.Scalars3, error) {
	m := new(matrix3.Scalars3)
	var a codec.Arena
	a.Grow(len(wire))
	d := codec.Decoder{Buf: wire}
	if err := unmarshalScalars(&d, &a, m, 0); err != nil {
		return nil, err
	}
	return m, nil
}

// ---------------------------------------------------------------------------
// Tier gate: identical payloads to pbench compare_test fill().
// ---------------------------------------------------------------------------

func i32p(v int32) *int32   { return &v }
func strp(v string) *string { return &v }

func fillOracle(n int) *matrix3pb.Scalars3 {
	m := &matrix3pb.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64, U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64, Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: matrix3pb.Color3_COLOR3_GREEN,
		Inner:  &matrix3pb.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7), OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3pb.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func fillGen(n int) *matrix3.Scalars3 {
	m := &matrix3.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64, U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64, Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: matrix3.Color3_COLOR3_GREEN,
		Inner:  &matrix3.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7), OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func checkFields(t *testing.T, label string, got *matrix3.Scalars3, want *matrix3pb.Scalars3) {
	t.Helper()
	if got.I32 != want.I32 || got.I64 != want.I64 || got.U32 != want.U32 || got.U64 != want.U64 ||
		got.S32 != want.S32 || got.S64 != want.S64 || got.Fx32 != want.Fx32 || got.Fx64 != want.Fx64 ||
		got.Sfx32 != want.Sfx32 || got.Sfx64 != want.Sfx64 || got.F32 != want.F32 || got.F64 != want.F64 ||
		got.Flag != want.Flag || got.Str != want.Str || int32(got.Color) != int32(want.Color) {
		t.Errorf("%s: scalar mismatch", label)
	}
	if got.Inner.GetName() != want.Inner.GetName() || got.Inner.GetValue() != want.Inner.GetValue() {
		t.Errorf("%s: inner mismatch", label)
	}
	if (got.OptI32 == nil) != (want.OptI32 == nil) ||
		(got.OptI32 != nil && *got.OptI32 != *want.OptI32) {
		t.Errorf("%s: opt_i32 mismatch", label)
	}
	if (got.OptStr == nil) != (want.OptStr == nil) ||
		(got.OptStr != nil && *got.OptStr != *want.OptStr) {
		t.Errorf("%s: opt_str mismatch", label)
	}
	if len(got.Nums) != len(want.Nums) || len(got.Tags) != len(want.Tags) ||
		len(got.Flags) != len(want.Flags) || len(got.Inners) != len(want.Inners) {
		t.Fatalf("%s: repeated length mismatch", label)
	}
	for i := range want.Nums {
		if got.Nums[i] != want.Nums[i] || got.Tags[i] != want.Tags[i] ||
			got.Flags[i] != want.Flags[i] || got.Inners[i].Name != want.Inners[i].Name ||
			got.Inners[i].Value != want.Inners[i].Value {
			t.Fatalf("%s: repeated element %d mismatch", label, i)
		}
	}
}

func TestTiersByteParity(t *testing.T) {
	tiers := []struct {
		label   string
		n, want int
	}{
		{"136B", 0, 136},
		{"174B", 1, 174},
		{"2469B", 100, 2469},
		{"74913B", 3000, 74913},
	}
	for _, tr := range tiers {
		want := fillOracle(tr.n)
		ref, err := proto.Marshal(want)
		if err != nil {
			t.Fatal(err)
		}
		if len(ref) != tr.want {
			t.Fatalf("%s: official wrote %d bytes, want %d", tr.label, len(ref), tr.want)
		}
		got, err := encodeScalars(fillGen(tr.n))
		if err != nil {
			t.Fatalf("%s: encode: %v", tr.label, err)
		}
		if string(got) != string(ref) {
			t.Fatalf("%s: bytes differ from protobuf-go (%d vs %d)", tr.label, len(got), len(ref))
		}
		back, err := decodeScalars(ref)
		if err != nil {
			t.Fatalf("%s: decode: %v", tr.label, err)
		}
		checkFields(t, tr.label, back, want)
		// Empty message: empty bytes both ways.
		empty, err := encodeScalars(new(matrix3.Scalars3))
		if err != nil || len(empty) != 0 {
			t.Fatalf("%s: empty message wrote %d bytes, err=%v", tr.label, len(empty), err)
		}
		zero, err := decodeScalars(nil)
		if err != nil {
			t.Fatalf("%s: empty decode: %v", tr.label, err)
		}
		checkFields(t, tr.label+" zero", zero, new(matrix3pb.Scalars3))
	}
}

// Unknown enum values survive the round trip as int32.
func TestEnumUnknown(t *testing.T) {
	gen := &matrix3.Scalars3{Color: matrix3.Color3(99)}
	got, err := encodeScalars(gen)
	if err != nil {
		t.Fatal(err)
	}
	oracle := &matrix3pb.Scalars3{Color: matrix3pb.Color3(99)}
	ref, err := proto.Marshal(oracle)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(ref) {
		t.Fatal("unknown enum bytes differ")
	}
	back, err := decodeScalars(ref)
	if err != nil {
		t.Fatal(err)
	}
	if back.Color != 99 {
		t.Fatalf("unknown enum decoded as %d", int32(back.Color))
	}
}

// ---------------------------------------------------------------------------
// Oneof + map coverage via dynamicpb oracle (protoc-generated descriptors).
// ---------------------------------------------------------------------------

func loadMessage(t *testing.T, path, msgName string) protoreflect.MessageDescriptor {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var fds descriptorpb.FileDescriptorSet
	if err := proto.Unmarshal(raw, &fds); err != nil {
		t.Fatal(err)
	}
	for _, fdp := range fds.File {
		fd, err := protodesc.NewFile(fdp, nil)
		if err != nil {
			t.Fatal(err)
		}
		if md := fd.Messages().ByName(protoreflect.Name(msgName)); md != nil {
			return md
		}
	}
	t.Fatalf("message %s not found in %s", msgName, path)
	return nil
}

// Oneof model in codegen shape: interface plus wrapper structs.
type isOneof1Kind interface{ isOneof1Kind() }

type oneof1Num struct{ Num int32 }

func (*oneof1Num) isOneof1Kind() {}

type oneof1Text struct{ Text string }

func (*oneof1Text) isOneof1Kind() {}

type oneof1Inner struct{ V int32 }

type oneof1InnerWrap struct{ Inner *oneof1Inner }

func (*oneof1InnerWrap) isOneof1Kind() {}

type oneof1Msg struct {
	Name string
	Kind isOneof1Kind
}

func marshalOneof1Inner(e *codec.Encoder, m *oneof1Inner, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	if m.V != 0 {
		e.Tag(1, codec.WireVarint)
		e.Varint(uint64(int64(m.V)))
	}
	return nil
}

func unmarshalOneof1Inner(d *codec.Decoder, m *oneof1Inner, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		switch num {
		case 1:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.V = int32(v)
		default:
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

func marshalOneof1(e *codec.Encoder, m *oneof1Msg, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	if m.Name != "" {
		e.Tag(1, codec.WireLen)
		e.Text(m.Name)
	}
	switch x := m.Kind.(type) {
	case *oneof1Num:
		e.Tag(2, codec.WireVarint)
		e.Varint(uint64(int64(x.Num)))
	case *oneof1Text:
		e.Tag(3, codec.WireLen)
		e.Text(x.Text)
	case *oneof1InnerWrap:
		mark := e.Begin(4)
		if err := marshalOneof1Inner(e, x.Inner, depth+1); err != nil {
			return err
		}
		e.End(mark)
	}
	return nil
}

func unmarshalOneof1(d *codec.Decoder, a *codec.Arena, m *oneof1Msg, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		switch num {
		case 1:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Name = a.AddString(raw)
		case 2:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.Kind = &oneof1Num{Num: int32(v)}
		case 3:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Kind = &oneof1Text{Text: a.AddString(raw)}
		case 4:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			w := &oneof1InnerWrap{Inner: new(oneof1Inner)}
			sub := codec.Decoder{Buf: raw}
			if err := unmarshalOneof1Inner(&sub, w.Inner, depth+1); err != nil {
				return err
			}
			m.Kind = w
		default:
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

func oneof1Oracle(t *testing.T, name string, kind string, num int32, text string, inner int32) *dynamicpb.Message {
	t.Helper()
	md := loadMessage(t, "testdata/oneof1.desc", "Oneof1")
	msg := dynamicpb.NewMessage(md)
	if name != "" {
		msg.Set(md.Fields().ByName("name"), protoreflect.ValueOfString(name))
	}
	switch kind {
	case "num":
		msg.Set(md.Fields().ByNumber(2), protoreflect.ValueOfInt32(num))
	case "text":
		msg.Set(md.Fields().ByNumber(3), protoreflect.ValueOfString(text))
	case "inner":
		sub := dynamicpb.NewMessage(md.Fields().ByNumber(4).Message())
		if inner != 0 {
			sub.Set(sub.Descriptor().Fields().ByName("v"), protoreflect.ValueOfInt32(inner))
		}
		msg.Set(md.Fields().ByNumber(4), protoreflect.ValueOfMessage(sub))
	}
	return msg
}

func TestOneofRoundTrip(t *testing.T) {
	cases := []struct {
		label string
		msg   *oneof1Msg
		kind  string
		num   int32
		text  string
		inner int32
	}{
		{"num", &oneof1Msg{Name: "n", Kind: &oneof1Num{Num: -42}}, "num", -42, "", 0},
		{"text", &oneof1Msg{Kind: &oneof1Text{Text: "hi"}}, "text", 0, "hi", 0},
		{"inner", &oneof1Msg{Name: "x", Kind: &oneof1InnerWrap{Inner: &oneof1Inner{V: -7}}}, "inner", 0, "", -7},
		{"empty-inner", &oneof1Msg{Kind: &oneof1InnerWrap{Inner: &oneof1Inner{}}}, "inner", 0, "", 0},
		{"none", &oneof1Msg{Name: "only"}, "", 0, "", 0},
	}
	for _, tc := range cases {
		want := oneof1Oracle(t, tc.msg.Name, tc.kind, tc.num, tc.text, tc.inner)
		// Deterministic: dynamicpb marshals fields in an unstable order,
		// so plain proto.Marshal is flaky as an oracle here.
		ref, err := proto.MarshalOptions{Deterministic: true}.Marshal(want)
		if err != nil {
			t.Fatal(err)
		}
		e := codec.Encoder{}
		if err := marshalOneof1(&e, tc.msg, 0); err != nil {
			t.Fatalf("%s: %v", tc.label, err)
		}
		if string(e.Result()) != string(ref) {
			t.Fatalf("%s: bytes differ from protobuf-go", tc.label)
		}
		var a codec.Arena
		a.Grow(len(ref))
		d := codec.Decoder{Buf: ref}
		back := new(oneof1Msg)
		if err := unmarshalOneof1(&d, &a, back, 0); err != nil {
			t.Fatalf("%s: decode: %v", tc.label, err)
		}
		if back.Name != tc.msg.Name {
			t.Fatalf("%s: name %q want %q", tc.label, back.Name, tc.msg.Name)
		}
		switch w := tc.msg.Kind.(type) {
		case *oneof1Num:
			g, ok := back.Kind.(*oneof1Num)
			if !ok || g.Num != w.Num {
				t.Fatalf("%s: kind mismatch %+v", tc.label, back.Kind)
			}
		case *oneof1Text:
			g, ok := back.Kind.(*oneof1Text)
			if !ok || g.Text != w.Text {
				t.Fatalf("%s: kind mismatch %+v", tc.label, back.Kind)
			}
		case *oneof1InnerWrap:
			g, ok := back.Kind.(*oneof1InnerWrap)
			if !ok || g.Inner.V != w.Inner.V {
				t.Fatalf("%s: kind mismatch %+v", tc.label, back.Kind)
			}
		default:
			if back.Kind != nil {
				t.Fatalf("%s: kind %+v want nil", tc.label, back.Kind)
			}
		}
		// Reverse: our bytes parse as the same dynamic message.
		rt := dynamicpb.NewMessage(want.Descriptor())
		if err := proto.Unmarshal(e.Result(), rt); err != nil {
			t.Fatalf("%s: oracle reparse: %v", tc.label, err)
		}
		if !proto.Equal(want, rt) {
			t.Fatalf("%s: oracle reparse mismatch", tc.label)
		}
	}
}

// Map model in codegen-entry shape: Go maps over entry encode/decode.
type mapValue struct {
	Name  string
	Value int32
}

type mapTest struct {
	ByInt map[int32]string
	ByStr map[string]int32
	ByMsg map[string]*mapValue
}

func marshalMapValue(e *codec.Encoder, m *mapValue, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	if m.Name != "" {
		e.Tag(1, codec.WireLen)
		e.Text(m.Name)
	}
	if m.Value != 0 {
		e.Tag(2, codec.WireVarint)
		e.Varint(uint64(int64(m.Value)))
	}
	return nil
}

func unmarshalMapValue(d *codec.Decoder, a *codec.Arena, m *mapValue, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		switch num {
		case 1:
			if wt != codec.WireLen {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			raw, err := d.Bytes()
			if err != nil {
				return err
			}
			m.Name = a.AddString(raw)
		case 2:
			if wt != codec.WireVarint {
				if err := d.Skip(num, wt, depth); err != nil {
					return err
				}
				continue
			}
			v, err := d.Varint()
			if err != nil {
				return err
			}
			m.Value = int32(v)
		default:
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

func marshalMapTest(e *codec.Encoder, m *mapTest, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	if m == nil {
		return nil
	}
	// Field-number order with sorted keys, matching protobuf-go output.
	intKeys := make([]int32, 0, len(m.ByInt))
	for k := range m.ByInt {
		intKeys = append(intKeys, k)
	}
	sort.Slice(intKeys, func(i, j int) bool { return intKeys[i] < intKeys[j] })
	for _, k := range intKeys {
		mark := e.Begin(1)
		e.Tag(1, codec.WireVarint)
		e.Varint(uint64(int64(k)))
		e.Tag(2, codec.WireLen)
		e.Text(m.ByInt[k])
		e.End(mark)
	}
	strKeys := make([]string, 0, len(m.ByStr))
	for k := range m.ByStr {
		strKeys = append(strKeys, k)
	}
	sort.Strings(strKeys)
	for _, k := range strKeys {
		mark := e.Begin(2)
		e.Tag(1, codec.WireLen)
		e.Text(k)
		e.Tag(2, codec.WireVarint)
		e.Varint(uint64(int64(m.ByStr[k])))
		e.End(mark)
	}
	msgKeys := make([]string, 0, len(m.ByMsg))
	for k := range m.ByMsg {
		msgKeys = append(msgKeys, k)
	}
	sort.Strings(msgKeys)
	for _, k := range msgKeys {
		mark := e.Begin(3)
		e.Tag(1, codec.WireLen)
		e.Text(k)
		sub := e.Begin(2)
		if err := marshalMapValue(e, m.ByMsg[k], depth+1); err != nil {
			return err
		}
		e.End(sub)
		e.End(mark)
	}
	return nil
}

func unmarshalMapTest(d *codec.Decoder, a *codec.Arena, m *mapTest, depth int) error {
	if err := codec.CheckDepth(depth); err != nil {
		return err
	}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			return err
		}
		if wt != codec.WireLen {
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
			continue
		}
		raw, err := d.Bytes()
		if err != nil {
			return err
		}
		sub := codec.Decoder{Buf: raw}
		switch num {
		case 1:
			var k int32
			var v string
			for !sub.Done() {
				en, ewt, err := sub.Next()
				if err != nil {
					return err
				}
				switch en {
				case 1:
					if ewt != codec.WireVarint {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					kv, err := sub.Varint()
					if err != nil {
						return err
					}
					k = int32(kv)
				case 2:
					if ewt != codec.WireLen {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					rv, err := sub.Bytes()
					if err != nil {
						return err
					}
					v = a.AddString(rv)
				default:
					if err := sub.Skip(en, ewt, depth+1); err != nil {
						return err
					}
				}
			}
			if m.ByInt == nil {
				m.ByInt = map[int32]string{}
			}
			m.ByInt[k] = v
		case 2:
			var k string
			var v int32
			for !sub.Done() {
				en, ewt, err := sub.Next()
				if err != nil {
					return err
				}
				switch en {
				case 1:
					if ewt != codec.WireLen {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					rk, err := sub.Bytes()
					if err != nil {
						return err
					}
					k = a.AddString(rk)
				case 2:
					if ewt != codec.WireVarint {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					vv, err := sub.Varint()
					if err != nil {
						return err
					}
					v = int32(vv)
				default:
					if err := sub.Skip(en, ewt, depth+1); err != nil {
						return err
					}
				}
			}
			if m.ByStr == nil {
				m.ByStr = map[string]int32{}
			}
			m.ByStr[k] = v
		case 3:
			var k string
			mv := new(mapValue)
			for !sub.Done() {
				en, ewt, err := sub.Next()
				if err != nil {
					return err
				}
				switch en {
				case 1:
					if ewt != codec.WireLen {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					rk, err := sub.Bytes()
					if err != nil {
						return err
					}
					k = a.AddString(rk)
				case 2:
					if ewt != codec.WireLen {
						if err := sub.Skip(en, ewt, depth+1); err != nil {
							return err
						}
						continue
					}
					rv, err := sub.Bytes()
					if err != nil {
						return err
					}
					sd := codec.Decoder{Buf: rv}
					if err := unmarshalMapValue(&sd, a, mv, depth+2); err != nil {
						return err
					}
				default:
					if err := sub.Skip(en, ewt, depth+1); err != nil {
						return err
					}
				}
			}
			if m.ByMsg == nil {
				m.ByMsg = map[string]*mapValue{}
			}
			m.ByMsg[k] = mv
		default:
			if err := d.Skip(num, wt, depth); err != nil {
				return err
			}
		}
	}
	return nil
}

func mapOracle(t *testing.T, m *mapTest) *dynamicpb.Message {
	t.Helper()
	md := loadMessage(t, "testdata/maptest.desc", "MapTest")
	msg := dynamicpb.NewMessage(md)
	if len(m.ByInt) > 0 {
		mp := msg.NewField(md.Fields().ByNumber(1)).Map()
		for k, v := range m.ByInt {
			mp.Set(protoreflect.ValueOfInt32(k).MapKey(), protoreflect.ValueOfString(v))
		}
		msg.Set(md.Fields().ByNumber(1), protoreflect.ValueOfMap(mp))
	}
	if len(m.ByStr) > 0 {
		mp := msg.NewField(md.Fields().ByNumber(2)).Map()
		for k, v := range m.ByStr {
			mp.Set(protoreflect.ValueOfString(k).MapKey(), protoreflect.ValueOfInt32(v))
		}
		msg.Set(md.Fields().ByNumber(2), protoreflect.ValueOfMap(mp))
	}
	if len(m.ByMsg) > 0 {
		mp := msg.NewField(md.Fields().ByNumber(3)).Map()
		valMD := md.Fields().ByNumber(3).Message().Fields().ByNumber(2).Message()
		for k, v := range m.ByMsg {
			sub := dynamicpb.NewMessage(valMD)
			if v.Name != "" {
				sub.Set(valMD.Fields().ByName("name"), protoreflect.ValueOfString(v.Name))
			}
			if v.Value != 0 {
				sub.Set(valMD.Fields().ByName("value"), protoreflect.ValueOfInt32(v.Value))
			}
			mp.Set(protoreflect.ValueOfString(k).MapKey(), protoreflect.ValueOfMessage(sub))
		}
		msg.Set(md.Fields().ByNumber(3), protoreflect.ValueOfMap(mp))
	}
	return msg
}

func TestMapRoundTrip(t *testing.T) {
	full := &mapTest{
		ByInt: map[int32]string{-1: "neg", 0: "", 7: "seven"},
		ByStr: map[string]int32{"": 1, "a": -2},
		ByMsg: map[string]*mapValue{"k": {Name: "v", Value: 3}, "empty": {}},
	}
	empty := &mapTest{}
	for _, tc := range []struct {
		label string
		msg   *mapTest
	}{
		{"full", full},
		{"empty", empty},
	} {
		want := mapOracle(t, tc.msg)
		ref, err := proto.MarshalOptions{Deterministic: true}.Marshal(want)
		if err != nil {
			t.Fatal(err)
		}
		e := codec.Encoder{}
		if err := marshalMapTest(&e, tc.msg, 0); err != nil {
			t.Fatalf("%s: %v", tc.label, err)
		}
		if string(e.Result()) != string(ref) {
			t.Fatalf("%s: bytes differ from protobuf-go", tc.label)
		}
		var a codec.Arena
		a.Grow(len(ref))
		d := codec.Decoder{Buf: ref}
		back := new(mapTest)
		if err := unmarshalMapTest(&d, &a, back, 0); err != nil {
			t.Fatalf("%s: decode: %v", tc.label, err)
		}
		if len(back.ByInt) != len(tc.msg.ByInt) || len(back.ByStr) != len(tc.msg.ByStr) ||
			len(back.ByMsg) != len(tc.msg.ByMsg) {
			t.Fatalf("%s: map length mismatch", tc.label)
		}
		for k, v := range tc.msg.ByInt {
			if back.ByInt[k] != v {
				t.Fatalf("%s: by_int[%d] mismatch", tc.label, k)
			}
		}
		for k, v := range tc.msg.ByStr {
			if back.ByStr[k] != v {
				t.Fatalf("%s: by_str[%q] mismatch", tc.label, k)
			}
		}
		for k, v := range tc.msg.ByMsg {
			if back.ByMsg[k].Name != v.Name || back.ByMsg[k].Value != v.Value {
				t.Fatalf("%s: by_msg[%q] mismatch", tc.label, k)
			}
		}
		rt := dynamicpb.NewMessage(want.Descriptor())
		if err := proto.Unmarshal(e.Result(), rt); err != nil {
			t.Fatalf("%s: oracle reparse: %v", tc.label, err)
		}
		if !proto.Equal(want, rt) {
			t.Fatalf("%s: oracle reparse mismatch", tc.label)
		}
	}
}

// ---------------------------------------------------------------------------
// Adversarial decode: errors, never panics.
// ---------------------------------------------------------------------------

func nestMessage(t *testing.T, levels int) []byte {
	t.Helper()
	// Group start tags nested `levels` deep: Skip recurses per level, so
	// this must trip ErrDepth end to end.
	var b []byte
	for i := 0; i < levels; i++ {
		b = codec.AppendTag(b, 1, codec.WireStartGroup)
	}
	return b
}

func TestAdversarial(t *testing.T) {
	cases := []struct {
		label string
		wire  func() []byte
	}{
		{"truncated varint", func() []byte { return []byte{8, 0x80} }},
		{"truncated tag", func() []byte { return []byte{0x80} }},
		{"truncated len", func() []byte { return append(codec.AppendTag(nil, 14, codec.WireLen), 5, 'a', 'b') }},
		{"len overrun", func() []byte { return append(codec.AppendTag(nil, 14, codec.WireLen), 100, 'a') }},
		{"field zero", func() []byte { return codec.AppendTag(nil, 0, codec.WireVarint) }},
		{"wiretype 6", func() []byte { return codec.AppendTag(nil, 1, 6) }},
		{"wiretype 7", func() []byte { return codec.AppendTag(nil, 1, 7) }},
		{"end group top", func() []byte { return codec.AppendTag(nil, 1, codec.WireEndGroup) }},
		{"mismatched end", func() []byte {
			b := codec.AppendTag(nil, 1, codec.WireStartGroup)
			return codec.AppendTag(b, 2, codec.WireEndGroup)
		}},
		{"truncated fixed32", func() []byte {
			b := codec.AppendTag(nil, 7, codec.WireFixed32)
			return append(b, 1, 2, 3)
		}},
		{"truncated fixed64", func() []byte {
			b := codec.AppendTag(nil, 8, codec.WireFixed64)
			return append(b, 1, 2, 3)
		}},
		{"bad packed", func() []byte {
			b := codec.AppendTag(nil, 17, codec.WireLen)
			return codec.AppendLen(b, []byte{0x80})
		}},
		{"varint 10byte cont", func() []byte {
			b := codec.AppendTag(nil, 1, codec.WireVarint)
			return append(b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80)
		}},
		{"group depth bomb", func() []byte { return nestMessage(t, codec.MaxDepth+10) }},
	}
	for _, tc := range cases {
		wire := tc.wire()
		if _, err := decodeScalars(wire); err == nil {
			t.Errorf("%s: want error, got nil", tc.label)
		}
	}
	// Unknown fields are skipped, known fields still land.
	e := codec.Encoder{}
	e.Tag(99, codec.WireVarint)
	e.Varint(12345)
	e.Tag(1, codec.WireVarint)
	e.Varint(7)
	e.Tag(100, codec.WireLen)
	e.Data([]byte("junk"))
	back, err := decodeScalars(e.Result())
	if err != nil {
		t.Fatalf("unknown skip: %v", err)
	}
	if back.I32 != 7 {
		t.Fatalf("unknown skip: I32=%d want 7", back.I32)
	}
	// Unpacked singles decode for packed fields (protobuf-go accepts both).
	e2 := codec.Encoder{}
	e2.Tag(17, codec.WireVarint)
	e2.Varint(3)
	e2.Tag(22, codec.WireVarint)
	e2.Varint(1)
	back2, err := decodeScalars(e2.Result())
	if err != nil {
		t.Fatal(err)
	}
	if len(back2.Nums) != 1 || back2.Nums[0] != 3 || len(back2.Flags) != 1 || !back2.Flags[0] {
		t.Fatalf("unpacked singles: %+v", back2)
	}
	// Group skip: unknown group bytes do not disturb the message.
	e3 := codec.Encoder{}
	e3.Tag(50, codec.WireStartGroup)
	e3.Tag(1, codec.WireVarint)
	e3.Varint(9)
	e3.Tag(50, codec.WireEndGroup)
	e3.Tag(2, codec.WireVarint)
	e3.Varint(11)
	back3, err := decodeScalars(e3.Result())
	if err != nil {
		t.Fatal(err)
	}
	if back3.I64 != 11 {
		t.Fatalf("group skip: I64=%d want 11", back3.I64)
	}
}

// No fixture schema is recursive (matrix3/oneof1/maptest nest at most one
// message deep), so message-depth overflow is probed at the contract level:
// generated code threads depth and calls CheckDepth on entry. Group-depth
// overflow goes through Skip end to end (see the table above).
func TestDepthLimit(t *testing.T) {
	if err := codec.CheckDepth(codec.MaxDepth); err != nil {
		t.Fatalf("limit itself must pass: %v", err)
	}
	if err := codec.CheckDepth(codec.MaxDepth + 1); err != codec.ErrDepth {
		t.Fatalf("over limit must be ErrDepth, got %v", err)
	}
	tiny := append(codec.AppendTag(nil, 13, codec.WireVarint), 1)
	for _, depth := range []int{codec.MaxDepth + 1, codec.MaxDepth + 40} {
		d := codec.Decoder{Buf: tiny}
		var a codec.Arena
		if err := unmarshalScalars(&d, &a, new(matrix3.Scalars3), depth); err != codec.ErrDepth {
			t.Fatalf("decode at depth %d: got %v, want ErrDepth", depth, err)
		}
		e := codec.Encoder{}
		if err := marshalScalars(&e, &matrix3.Scalars3{Flag: true}, depth); err != codec.ErrDepth {
			t.Fatalf("encode at depth %d: got %v, want ErrDepth", depth, err)
		}
	}
	// One below the limit still decodes.
	d := codec.Decoder{Buf: tiny}
	var a codec.Arena
	m := new(matrix3.Scalars3)
	if err := unmarshalScalars(&d, &a, m, codec.MaxDepth); err != nil {
		t.Fatalf("decode at limit: %v", err)
	}
	if !m.Flag {
		t.Fatal("decode at limit dropped the field")
	}
}

// Group framing round trip, using the pattern from codec.go's doc comment.
// The plan marks proto2 groups (Group=true, wire start/end); generated code
// emits Tag(n, StartGroup) ... Tag(n, EndGroup) around the member fields.
func TestGroupRoundTrip(t *testing.T) {
	e := codec.Encoder{}
	e.Tag(1, codec.WireStartGroup)
	e.Tag(2, codec.WireVarint)
	e.Varint(codec.ZigZagEncode32(-5))
	e.Tag(3, codec.WireLen)
	e.Text("hi")
	e.Tag(1, codec.WireEndGroup)
	e.Tag(7, codec.WireVarint)
	e.Varint(1)
	wire := e.Result()

	var x int32
	var s string
	var after bool
	d := codec.Decoder{Buf: wire}
	const depth = 0
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			t.Fatal(err)
		}
		if num == 1 && wt == codec.WireStartGroup {
			for {
				in, iwt, err := d.Next()
				if err != nil {
					t.Fatal(err)
				}
				if iwt == codec.WireEndGroup {
					if in != 1 {
						t.Fatal(codec.ErrWire)
					}
					break
				}
				switch in {
				case 2:
					if iwt != codec.WireVarint {
						if err := d.Skip(in, iwt, depth+1); err != nil {
							t.Fatal(err)
						}
						continue
					}
					v, err := d.Varint()
					if err != nil {
						t.Fatal(err)
					}
					x = codec.ZigZagDecode32(v)
				case 3:
					if iwt != codec.WireLen {
						if err := d.Skip(in, iwt, depth+1); err != nil {
							t.Fatal(err)
						}
						continue
					}
					raw, err := d.Bytes()
					if err != nil {
						t.Fatal(err)
					}
					s = string(raw)
				default:
					if err := d.Skip(in, iwt, depth+1); err != nil {
						t.Fatal(err)
					}
				}
			}
			continue
		}
		if num == 7 && wt == codec.WireVarint {
			v, err := d.Varint()
			if err != nil {
				t.Fatal(err)
			}
			after = v != 0
			continue
		}
		if err := d.Skip(num, wt, depth); err != nil {
			t.Fatal(err)
		}
	}
	if x != -5 || s != "hi" || !after {
		t.Fatalf("group round trip: x=%d s=%q after=%v", x, s, after)
	}
}

// ---------------------------------------------------------------------------
// Wire unit tests: primitives, framing, arena.
// ---------------------------------------------------------------------------

func TestWirePrimitives(t *testing.T) {
	for _, v := range []uint64{0, 1, 127, 128, 300, 1<<32 - 1, 1<<64 - 1, math.MaxUint64} {
		b := codec.AppendVarint(nil, v)
		if len(b) != codec.SizeVarint(v) {
			t.Fatalf("SizeVarint(%d) = %d, encoded %d", v, codec.SizeVarint(v), len(b))
		}
		got, n, err := codec.ConsumeVarint(b)
		if err != nil || got != v || n != len(b) {
			t.Fatalf("varint %d round trip: %d %d %v", v, got, n, err)
		}
	}
	// Zigzag extremes.
	if codec.ZigZagEncode32(math.MinInt32) != 0xffffffff || codec.ZigZagDecode32(1) != -1 {
		t.Fatal("zigzag32")
	}
	if codec.ZigZagEncode64(math.MinInt64) != 0xffffffffffffffff || codec.ZigZagDecode64(1) != -1 {
		t.Fatal("zigzag64")
	}
	// Fixed round trips.
	b := codec.AppendFixed32(codec.AppendFixed64(nil, 0x0102030405060708), 0x01020304)
	v64, n, err := codec.ConsumeFixed64(b)
	if err != nil || v64 != 0x0102030405060708 || n != 8 {
		t.Fatal("fixed64")
	}
	v32, n, err := codec.ConsumeFixed32(b[8:])
	if err != nil || v32 != 0x01020304 || n != 4 {
		t.Fatal("fixed32")
	}
	// Tag size + reject rules.
	if codec.SizeTag(1, codec.WireVarint) != 1 || codec.SizeTag(2048, codec.WireLen) != 3 {
		t.Fatal("SizeTag")
	}
	if _, _, _, err := codec.ConsumeTag([]byte{0}); err == nil {
		t.Fatal("field 0 accepted")
	}
	// Multi-byte Begin/End: 200 payload bytes need a 2-byte length.
	e := codec.Encoder{}
	mark := e.Begin(19)
	for i := 0; i < 200; i++ {
		e.Varint(1)
	}
	e.End(mark)
	out := e.Result()
	var d codec.Decoder
	d.Buf = out
	num, wt, err := d.Next()
	if err != nil || num != 19 || wt != codec.WireLen {
		t.Fatalf("framing tag: %d %d %v", num, wt, err)
	}
	raw, err := d.Bytes()
	if err != nil || len(raw) != 200 || !d.Done() {
		t.Fatalf("framing body: %d %v done=%v", len(raw), err, d.Done())
	}
	// Encoder reuse keeps capacity and drops contents.
	capBefore := cap(e.Buf)
	e.Reset()
	if len(e.Buf) != 0 || cap(e.Buf) != capBefore {
		t.Fatal("Reset")
	}
	// Arena copies own their memory.
	var a codec.Arena
	a.Grow(8)
	src := []byte("hello")
	s1 := a.AddString(src)
	c1 := a.AddBytes(src)
	src[0] = 'X'
	if s1 != "hello" || string(c1) != "hello" {
		t.Fatal("arena aliasing input")
	}
	a.Reset()
	_ = s1
}

// ---------------------------------------------------------------------------
// Offset fast-path parity: the same rows run through closures and through
// Off/Has layout facts must produce identical bytes and structs. The fast
// table's closures panic, so any silent fallback to closures fails the
// test instead of passing quietly.
// ---------------------------------------------------------------------------

type isOffKind interface{ isOffKind() }

type offKindN struct{ N int32 }

func (*offKindN) isOffKind() {}

type offProbe struct {
	A int32
	B int64
	S string
	N []int32
	O *int32
	K isOffKind
}

var (
	offA = uint32(unsafe.Offsetof(offProbe{}.A))
	offB = uint32(unsafe.Offsetof(offProbe{}.B))
	offS = uint32(unsafe.Offsetof(offProbe{}.S))
	offN = uint32(unsafe.Offsetof(offProbe{}.N))
	offO = uint32(unsafe.Offsetof(offProbe{}.O))
)

func offPanic[T any]() func(any) T {
	return func(any) T { panic("offProbe: fell back to closure on a fast row") }
}

func offPanicSet[T any]() func(any, T) {
	return func(any, T) { panic("offProbe: fell back to closure on a fast row") }
}

func offProbeFields(fast, stub bool) []*codec.Field {
	mk := func(f *codec.Field) *codec.Field {
		if fast {
			f.Fast = true
		}
		return f
	}
	getI32 := func(m any) int32 { return m.(*offProbe).A }
	setI32 := func(m any, v int32) { m.(*offProbe).A = v }
	getI64 := func(m any) int64 { return m.(*offProbe).B }
	setI64 := func(m any, v int64) { m.(*offProbe).B = v }
	getStr := func(m any) string { return m.(*offProbe).S }
	getRep := func(m any) []int32 { return m.(*offProbe).N }
	getOpt := func(m any) *int32 { return m.(*offProbe).O }
	if stub {
		getI32, setI32 = offPanic[int32](), offPanicSet[int32]()
		getI64, setI64 = offPanic[int64](), offPanicSet[int64]()
		getStr = offPanic[string]()
		getRep = offPanic[[]int32]()
		getOpt = offPanic[*int32]()
	}
	return []*codec.Field{
		mk(&codec.Field{Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32,
			Off:    offA,
			GetI32: getI32,
			SetI32: setI32}),
		mk(&codec.Field{Num: 2, Wire: codec.WireVarint, Kind: codec.KindInt64,
			Off:    offB,
			GetI64: getI64,
			SetI64: setI64}),
		mk(&codec.Field{Num: 3, Wire: codec.WireLen, Kind: codec.KindString,
			Off:    offS,
			GetStr: getStr,
			SetStr: func(m any, v string) { m.(*offProbe).S = v }}),
		mk(&codec.Field{Num: 4, Wire: codec.WireVarint, Kind: codec.KindInt32,
			Repeated: true, Packed: true,
			Off:       offN,
			GetRepI32: getRep,
			AppendI32: func(m any, v int32) { m.(*offProbe).N = append(m.(*offProbe).N, v) }}),
		mk(&codec.Field{Num: 5, Wire: codec.WireVarint, Kind: codec.KindInt32,
			Explicit: true,
			Off:      offO, Has: offO,
			GetOptI32: getOpt,
			SetOptI32: func(m any, v int32) { tmp := v; m.(*offProbe).O = &tmp }}),
		&codec.Field{Num: 6, Wire: codec.WireVarint, Kind: codec.KindInt32,
			Oneof: true,
			GetOneof: func(m any) (any, bool) {
				if x, ok := m.(*offProbe).K.(*offKindN); ok {
					return x.N, true
				}
				return nil, false
			},
			SetOneof: func(m any, v any) { m.(*offProbe).K = &offKindN{N: v.(int32)} }},
	}
}

func encodeProbe(t *codec.Table, m *offProbe) []byte {
	e := &codec.Encoder{Buf: make([]byte, 0, 64)}
	if err := codec.EncodeTable(e, m, t, 0); err != nil {
		panic(err)
	}
	return e.Result()
}

func decodeProbe(t *codec.Table, w []byte) *offProbe {
	m := &offProbe{}
	d := &codec.Decoder{Buf: w}
	a := &codec.Arena{}
	if err := codec.DecodeTable(d, a, m, t, 0); err != nil {
		panic(err)
	}
	return m
}

func TestOffsetFastPathParity(t *testing.T) {
	slow := codec.NewTable("probe", offProbeFields(false, false))
	fast := codec.NewTable("probe", offProbeFields(true, true))

	v := int32(-7)
	cases := map[string]*offProbe{
		"zero":      {},
		"full":      {A: -1, B: math.MinInt64, S: "hi", N: []int32{1, 2, 3}, O: &v, K: &offKindN{N: 9}},
		"nilopt":    {A: 5, S: "x"},
		"oneofzero": {A: 1, K: &offKindN{}},
	}
	for name, want := range cases {
		encSlow, encFast := encodeProbe(slow, want), encodeProbe(fast, want)
		if !bytes.Equal(encSlow, encFast) {
			t.Fatalf("%s: encode differs %x vs %x", name, encSlow, encFast)
		}
		decSlow, decFast := decodeProbe(slow, encSlow), decodeProbe(fast, encFast)
		if !reflect.DeepEqual(decSlow, decFast) {
			t.Fatalf("%s: decode differs %+v vs %+v", name, decSlow, decFast)
		}
		if !reflect.DeepEqual(decFast, want) {
			t.Fatalf("%s: round trip lost data: %+v", name, decFast)
		}
	}

	// Unknown fields are dropped on the struct path either way.
	withUnknown := append(encodeProbe(slow, cases["full"]),
		0x98, 0x06, 0x07) // field 99, varint 7
	if !reflect.DeepEqual(decodeProbe(slow, withUnknown), decodeProbe(fast, withUnknown)) {
		t.Fatal("unknown: decode differs")
	}

	// A misaligned 64-bit offset must fall back to closures, not fault.
	// The fallback table keeps working closures for that row.
	mis := codec.NewTable("probe", offProbeFields(true, false))
	mis.Fields[1].Off = 1
	want := cases["full"]
	if enc := encodeProbe(mis, want); !bytes.Equal(enc, encodeProbe(slow, want)) {
		t.Fatal("misaligned: encode differs")
	}
	if dec := decodeProbe(mis, encodeProbe(slow, want)); !reflect.DeepEqual(dec, want) {
		t.Fatalf("misaligned: decode lost data: %+v", dec)
	}
}
