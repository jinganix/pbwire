// Package pbwire is a minimal cgo binding for the pbwire C library: it
// builds a schema by hand, encodes maps of scalar fields, and decodes them
// back. It proves the ABI path end to end; anything richer belongs in a real
// host binding.
//
// Build the library first so the linker finds it:
//
//	make shared	# build/libpbwire.so.1 (Linux) / .1.dylib (macOS)
package pbwire

// #cgo CFLAGS: -I../../../include
// #cgo LDFLAGS: -L../../../build -lpbwire
// #cgo darwin LDFLAGS: -Wl,-rpath,${SRCDIR}/../../../build
// #cgo linux LDFLAGS: -Wl,-rpath,${SRCDIR}/../../../build
// #include <pbwire.h>
// #include <stdlib.h>
import "C"

import (
	"errors"
	"math"
	"unsafe"
)

// Types mirrors the pbw_type numbering for the scalar kinds this binding
// covers.
const (
	TDouble   = 1
	TFloat    = 2
	TInt64    = 3
	TUInt64   = 4
	TInt32    = 5
	TFixed64  = 6
	TFixed32  = 7
	TBool     = 8
	TString   = 9
	TBytes    = 12
	TUInt32   = 13
	TSFixed32 = 15
	TSFixed64 = 16
	TSInt32   = 17
	TSInt64   = 18
)

// Field is one scalar field of a hand-built message.
type Field struct {
	Name   string
	Number uint32
	Type   int
}

// Schema owns a pbw_schema and the descriptors built from it: the Schema
// must outlive every message, like in C.
type Schema struct {
	s *C.pbw_schema
}

// New opens an empty schema.
func New() *Schema {
	return &Schema{s: C.pbw_schema_new()}
}

// Close releases the schema.
func (s *Schema) Close() {
	if s.s != nil {
		C.pbw_schema_free(s.s)
		s.s = nil
	}
}

// ABI checks the runtime library against the header this file compiled with.
func (s *Schema) ABI() int {
	return int(C.pbw_abi_version())
}

// Message declares a message type and links the schema.
func (s *Schema) Message(name string, fields []Field) (*Message, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	desc := C.pbw_schema_add_message(s.s, cname)
	if desc == nil {
		return nil, errors.New("add message")
	}
	for _, f := range fields {
		cf := C.CString(f.Name)
		// shortcut: hand-built schemas here are trusted; a host binding
		// takes names from its own registry, upgrade to validation there.
		C.pbw_message_add_field(desc, cf, C.uint32_t(f.Number),
			C.pbw_type(f.Type), C.PBW_LABEL_OPTIONAL)
		C.free(unsafe.Pointer(cf))
	}
	if C.pbw_schema_link(s.s) != C.PBW_OK {
		return nil, errors.New("link")
	}
	cf := C.CString(name)
	defer C.free(unsafe.Pointer(cf))
	found := C.pbw_schema_find(s.s, cf)
	if found == nil {
		return nil, errors.New("find")
	}
	return &Message{desc: found, fields: fields}, nil
}

// Value is one scalar: ints and floats arrive as uint64 bit patterns,
// strings and bytes as raw payloads.
type Value struct {
	Uint  uint64
	Bytes []byte
	IsStr bool
}

// Message is a bound message type: Encode takes field number -> value,
// Decode returns it back.
type Message struct {
	desc   *C.pbw_descriptor
	fields []Field
}

func (m *Message) newValue(f Field, v Value) *C.pbw_value {
	switch f.Type {
	case TString:
		cs := C.CString(string(v.Bytes))
		defer C.free(unsafe.Pointer(cs))
		return C.pbw_value_new_string(cs)
	case TBytes:
		if len(v.Bytes) == 0 {
			return C.pbw_value_new_bytes(nil, 0)
		}
		return C.pbw_value_new_bytes(unsafe.Pointer(&v.Bytes[0]),
			C.size_t(len(v.Bytes)))
	case TBool:
		if v.Uint != 0 {
			return C.pbw_value_new_bool(1)
		}
		return C.pbw_value_new_bool(0)
	case TFloat:
		return C.pbw_value_new_float(C.float(
			float32FromBits(uint32(v.Uint))))
	case TDouble:
		return C.pbw_value_new_double(C.double(float64FromBits(v.Uint)))
	default:
		if isSigned(f.Type) && int64(v.Uint) < 0 {
			return C.pbw_value_new_int(C.int64_t(int64(v.Uint)))
		}
		return C.pbw_value_new_uint(C.uint64_t(v.Uint))
	}
}

func float32FromBits(u uint32) float32 { return math.Float32frombits(u) }

func float64FromBits(u uint64) float64 { return math.Float64frombits(u) }

func isSigned(t int) bool {
	switch t {
	case TInt32, TInt64, TSInt32, TSInt64:
		return true
	}
	return false
}

// Encode builds a message tree from values and serializes it.
func (m *Message) Encode(values map[uint32]Value) ([]byte, error) {
	msg := C.pbw_message_new(m.desc)
	if msg == nil {
		return nil, errors.New("new message")
	}
	defer C.pbw_message_free(msg)
	for _, f := range m.fields {
		v, ok := values[f.Number]
		if !ok {
			continue
		}
		val := m.newValue(f, v)
		if val == nil {
			return nil, errors.New("new value")
		}
		if C.pbw_message_set(msg, C.uint32_t(f.Number), val) != C.PBW_OK {
			C.pbw_value_free(val)
			return nil, errors.New(C.GoString(
				C.pbw_strerror(C.PBW_ERR_TYPE)))
		}
	}
	var out *C.uint8_t
	var length C.size_t
	if C.pbw_encode(msg, &out, &length) != C.PBW_OK {
		return nil, errors.New("encode")
	}
	defer C.pbw_free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(length)), nil
}

// Decode parses wire bytes into field number -> value.
func (m *Message) Decode(wire []byte) (map[uint32]Value, error) {
	var msg *C.pbw_message
	var p *C.uchar
	if len(wire) > 0 {
		p = (*C.uchar)(unsafe.Pointer(&wire[0]))
	}
	if C.pbw_decode(m.desc, unsafe.Pointer(p), C.size_t(len(wire)),
		&msg) != C.PBW_OK {
		return nil, errors.New("decode")
	}
	defer C.pbw_message_free(msg)
	result := make(map[uint32]Value)
	for _, f := range m.fields {
		got := C.pbw_message_get(msg, C.uint32_t(f.Number))
		if got == nil {
			continue
		}
		if C.pbw_value_get_kind(got) == C.PBW_VALUE_BYTES {
			var n C.size_t
			raw := C.pbw_value_get_bytes(got, &n)
			result[f.Number] = Value{
				Bytes: C.GoBytes(unsafe.Pointer(raw), C.int(n)),
				IsStr: f.Type == TString,
			}
		} else {
			result[f.Number] = Value{
				Uint: uint64(C.pbw_value_get_uint(got)),
			}
		}
	}
	return result, nil
}
