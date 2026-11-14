// Package dynamic is the MVP of a hot-swappable Go binding: it loads a
// FileDescriptorSet at run time (no recompile, no restart), builds dynamic
// message trees and encodes/decodes them through the shared C core over cgo.
//
// This is the counterpart to the static generator (../plugin): generated
// codecs stay the fast path for pinned schemas; this package is for schemas
// that must change under a running process. Correctness comes from the C
// core (conformance-tested); the per-op price is one cgo transition plus
// one heap copy each way, measured in ../cgoprobe.
package dynamic

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#cgo LDFLAGS: ${SRCDIR}/../../../build/libpbwire.a
#include <stdlib.h>
#include "pbwire.h"

// The visit callback crosses from C back into Go: the exported Go function
// below is the trampoline, and this wrapper casts it to pbw_visit_fn.
extern int pbwgoVisitTramp(void *ud, void *field, uint32_t number,
                           uint32_t wire_type, void *value);
static pbw_status pbwgo_visit(const pbw_descriptor *desc, const void *buf,
                             size_t len, const pbw_limits *limits,
                             uintptr_t ud, size_t *fail_off)
{
    return pbw_visit_limits(desc, buf, len, limits,
                            (pbw_visit_fn)pbwgoVisitTramp, (void *)ud,
                            fail_off);
}

*/
import "C"

import (
	"errors"
	"fmt"
	"math"
	"runtime"
	"runtime/cgo"
	"sync"
	"sync/atomic"
	"unsafe"
)

// PBW_* types mirrored for the fixture paths below.
const (
	TypeDouble   = 1
	TypeFloat    = 2
	TypeInt64    = 3
	TypeUint64   = 4
	TypeInt32    = 5
	TypeFixed64  = 6
	TypeFixed32  = 7
	TypeBool     = 8
	TypeString   = 9
	TypeMessage  = 11
	TypeBytes    = 12
	TypeUint32   = 13
	TypeEnum     = 14
	TypeSFixed32 = 15
	TypeSFixed64 = 16
	TypeSInt32   = 17
	TypeSInt64   = 18
)

func statusErr(op string, st C.pbw_status) error {
	if st == C.PBW_OK {
		return nil
	}
	return errors.New(op + ": " + C.GoString(C.pbw_strerror(st)))
}

// Schema is one loaded FileDescriptorSet. The C schema must outlive every
// message built from it; refcounting keeps that true across hot-swaps:
// in-flight messages retain their schema, Registry/probe code releases its
// own reference when done.
type Schema struct {
	c       *C.pbw_schema
	mu      sync.Mutex
	refs    int
	version uint64
}

// Load parses descSet (protoc --descriptor_set_out bytes) into a new Schema.
// The input is copied; the caller keeps it.
func Load(descSet []byte) (*Schema, error) {
	if len(descSet) == 0 {
		return nil, errors.New("dynamic: empty descriptor set")
	}
	cbuf := C.CBytes(descSet)
	defer C.free(cbuf)
	var out *C.pbw_schema
	if st := C.pbw_descriptor_set_load(cbuf, C.size_t(len(descSet)), &out); st != C.PBW_OK {
		if out != nil {
			msg := C.GoString(C.pbw_schema_error(out))
			C.pbw_schema_free(out)
			return nil, fmt.Errorf("dynamic: load: %s", msg)
		}
		return nil, statusErr("dynamic: load", st)
	}
	s := &Schema{c: out, refs: 1}
	runtime.SetFinalizer(s, func(s *Schema) { s.release() })
	return s, nil
}

func (s *Schema) retain() {
	s.mu.Lock()
	s.refs++
	s.mu.Unlock()
}

func (s *Schema) release() {
	s.mu.Lock()
	s.refs--
	free := s.refs == 0
	s.mu.Unlock()
	if free {
		C.pbw_schema_free(s.c)
	}
}

// Close drops the loader's reference. In-flight messages keep the C schema
// alive until they close.
func (s *Schema) Close() { s.release() }

// MessageType is a message descriptor bound to its schema.
type MessageType struct {
	s      *Schema
	c      *C.pbw_descriptor
	mu     sync.RWMutex
	fields map[string]*C.pbw_field
	own    bool // false for nested subtypes tied to a root message
}

// Message returns the named message type ("pkg.Outer.Inner", no leading dot).
func (s *Schema) Message(name string) (*MessageType, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	c := C.pbw_schema_find(s.c, cname)
	if c == nil {
		return nil, fmt.Errorf("dynamic: message %s not found", name)
	}
	s.retain()
	return &MessageType{s: s, c: c, fields: make(map[string]*C.pbw_field), own: true}, nil
}

// Close releases the type's schema reference; a no-op for subtypes owned
// by a root message (their schema reference dies with the root).
func (t *MessageType) Close() {
	if t.own {
		t.s.release()
	}
}

// subtype resolves a MESSAGE field's element type. It shares the root's
// schema reference, so it must never outlive the root message.
func (t *MessageType) subtype(f *C.pbw_field) *MessageType {
	return &MessageType{s: t.s, c: C.pbw_field_message_type(f), fields: make(map[string]*C.pbw_field)}
}

func (t *MessageType) field(name string) (*C.pbw_field, error) {
	t.mu.RLock()
	f, ok := t.fields[name]
	t.mu.RUnlock()
	if ok {
		return f, nil
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	f = C.pbw_descriptor_find_field_name(t.c, cname)
	if f == nil {
		return nil, fmt.Errorf("dynamic: field %s not found", name)
	}
	t.mu.Lock()
	t.fields[name] = f
	t.mu.Unlock()
	return f, nil
}

// Message is one dynamic tree. Roots own their tree and MUST be freed with
// an explicit Close: no finalizer on the hot path (explicit-close discipline).
// Nested views from NewMessage read into the root and must not be freed.
type Message struct {
	typ    *MessageType
	c      *C.pbw_message
	root   *Message // non-nil for views: keeps the root (and schema) alive
	own    bool
	closed bool // roots only: a view must not touch the freed arena
	// encCap is the buffer capacity the last encode needed, i.e. the size
	// hint for the next one: it lets an encode write straight into Go memory
	// with one downcall, no C malloc and no copy back. Advisory and atomic:
	// a stale (small) value only costs a retry, a stale (large) value only
	// allocates a few bytes too many, so concurrent Encode stays correct.
	encCap atomic.Int64
}

// New builds an empty root message.
func (t *MessageType) New() *Message {
	m := &Message{typ: t, c: C.pbw_message_new(t.c), own: true}
	t.s.retain()
	return m
}

// Close frees a root tree; idempotent, a no-op for views.
func (m *Message) Close() {
	if m == nil || !m.own || m.c == nil {
		return
	}
	h := m.c
	m.c = nil
	m.closed = true
	m.typ.s.release()
	C.pbw_message_free(h)
}

func (m *Message) store(f *C.pbw_field, v *C.pbw_value) error {
	if err := m.check(); err != nil {
		C.pbw_value_free(v)
		return err
	}
	if v == nil {
		return errors.New("dynamic: value allocation failed")
	}
	if st := C.pbw_message_set_field(m.c, f, v); st != C.PBW_OK {
		C.pbw_value_free(v) // ownership stays with the caller on failure
		return statusErr("dynamic: set", st)
	}
	return nil
}

// SetInt stores any integer/enum field as raw 64-bit bits (sign-extended,
// like the other bindings); repeated fields append.
func (m *Message) SetInt(name string, v int64) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	return m.store(f, C.pbw_value_new_int(C.int64_t(v)))
}

// SetUint stores an unsigned field's bit pattern.
func (m *Message) SetUint(name string, v uint64) error {
	return m.SetInt(name, int64(v))
}

// SetBool stores a bool field; repeated fields append.
func (m *Message) SetBool(name string, v bool) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	n := C.int(0)
	if v {
		n = 1
	}
	return m.store(f, C.pbw_value_new_bool(n))
}

// SetFloat stores a float field.
func (m *Message) SetFloat(name string, v float32) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	return m.store(f, C.pbw_value_new_float(C.float(v)))
}

// SetDouble stores a double field.
func (m *Message) SetDouble(name string, v float64) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	return m.store(f, C.pbw_value_new_double(C.double(v)))
}

// SetString stores a string field (UTF-8); repeated fields append.
func (m *Message) SetString(name string, v string) error {
	return m.SetBytes(name, []byte(v))
}

// SetBytes stores a string/bytes payload; C copies it.
func (m *Message) SetBytes(name string, v []byte) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	var v2 *C.pbw_value
	if len(v) == 0 {
		v2 = C.pbw_value_new_bytes(nil, 0)
	} else {
		cbuf := C.CBytes(v)
		defer C.free(cbuf)
		v2 = C.pbw_value_new_bytes(cbuf, C.size_t(len(v)))
	}
	return m.store(f, v2)
}

// SetMessageBytes parses wire bytes as the MESSAGE or GROUP field name and
// stores the result. It bridges generated packages: a foreign value marshals
// to bytes through its own package, then lands here without either package
// calling the other's unexported fill methods.
func (m *Message) SetMessageBytes(name string, wire []byte) error {
	f, err := m.typ.field(name)
	if err != nil {
		return err
	}
	if C.pbw_field_message_type(f) == nil {
		return fmt.Errorf("dynamic: field %s is not a message", name)
	}
	tmp, err := m.typ.subtype(f).Decode(wire)
	if err != nil {
		return err
	}
	v := C.pbw_value_new_message(tmp.c)
	if v == nil {
		tmp.Close()
		return errors.New("dynamic: value allocation failed")
	}
	// Ownership (tree and schema pin) moves into v; Close below is a no-op.
	tmp.c = nil
	tmp.typ.s.release()
	return m.store(f, v)
}

// NewMessage creates (or returns the existing) nested message for a MESSAGE
// field: singular replaces, repeated appends. The view writes straight into
// the root tree; never Close it separately.
func (m *Message) NewMessage(name string) (*Message, error) {
	if err := m.check(); err != nil {
		return nil, err
	}
	f, err := m.typ.field(name)
	if err != nil {
		return nil, err
	}
	child := C.pbw_message_set_message(m.c, C.uint32_t(C.pbw_field_number(f)))
	if child == nil {
		return nil, fmt.Errorf("dynamic: cannot create message field %s", name)
	}
	return &Message{typ: m.typ.subtype(f), c: child, root: m.rootOrSelf()}, nil
}

func (m *Message) rootOrSelf() *Message {
	if m.root != nil {
		return m.root
	}
	return m
}

// getValue resolves a field and returns its present value, or nil when the
// field is absent (never an error for absent: check Has first when it
// matters). Views into the tree keep the root alive through the caller.
// check reports a root that has already been closed. A view holds its root
// alive as a Go pointer, so the arena only goes away when Close runs; without
// this check a view would read freed memory.
func (m *Message) check() error {
	if m == nil {
		return errors.New("dynamic: nil message")
	}
	if r := m.rootOrSelf(); r.closed || r.c == nil {
		return errors.New("dynamic: message is closed")
	}
	return nil
}

func (m *Message) getValue(name string) (*C.pbw_value, error) {
	if err := m.check(); err != nil {
		return nil, err
	}
	f, err := m.typ.field(name)
	if err != nil {
		return nil, err
	}
	return C.pbw_message_get_field(m.c, f), nil
}

// Has reports whether the field carries a value on the wire.
func (m *Message) Has(name string) (bool, error) {
	v, err := m.getValue(name)
	if err != nil {
		return false, err
	}
	return v != nil, nil
}

// GetInt reads a signed integer/enum field (raw bits, like the setters).
func (m *Message) GetInt(name string) (int64, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return 0, false, err
	}
	return int64(C.pbw_value_get_int(v)), true, nil
}

// GetUint reads an unsigned integer field (raw bits).
func (m *Message) GetUint(name string) (uint64, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return 0, false, err
	}
	return uint64(C.pbw_value_get_uint(v)), true, nil
}

// GetBool reads a bool field.
func (m *Message) GetBool(name string) (bool, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return false, false, err
	}
	return C.pbw_value_get_bool(v) != 0, true, nil
}

// GetFloat reads a float field.
func (m *Message) GetFloat(name string) (float32, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return 0, false, err
	}
	return float32(C.pbw_value_get_float(v)), true, nil
}

// GetDouble reads a double field.
func (m *Message) GetDouble(name string) (float64, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return 0, false, err
	}
	return float64(C.pbw_value_get_double(v)), true, nil
}

// copyBytes copies a C buffer into Go, refusing a length that C.int cannot
// carry (a >2 GiB message would otherwise silently copy zero bytes).
func copyBytes(p unsafe.Pointer, l C.size_t) ([]byte, error) {
	if uint64(l) > math.MaxInt32 {
		return nil, fmt.Errorf("dynamic: %d bytes exceeds the cgo copy limit", uint64(l))
	}
	return C.GoBytes(p, C.int(l)), nil
}

// GetString reads a string field (copied out).
func (m *Message) GetString(name string) (string, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return "", false, err
	}
	if C.pbw_value_get_kind(v) != C.PBW_VALUE_BYTES {
		return "", false, fmt.Errorf("dynamic: field %q is not a string", name)
	}
	/* Exact bytes, not GoString: a protobuf string may contain NUL, and the
	 * C accessor stops there. Go strings carry it fine. */
	var l C.size_t
	p := C.pbw_value_get_bytes(v, &l)
	out, err := copyBytes(unsafe.Pointer(p), l)
	if err != nil {
		return "", false, err
	}
	return string(out), true, nil
}

// GetBytes reads a bytes field (copied out).
func (m *Message) GetBytes(name string) ([]byte, bool, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return nil, false, err
	}
	var l C.size_t
	p := C.pbw_value_get_bytes(v, &l)
	if C.pbw_value_get_kind(v) != C.PBW_VALUE_BYTES {
		return nil, false, fmt.Errorf("dynamic: field %q is not bytes", name)
	}
	out, err := copyBytes(unsafe.Pointer(p), l)
	if err != nil {
		return nil, false, err
	}
	return out, true, nil
}

// GetMessage reads a sub-message field as a view: Close is a no-op, the
// root keeps it alive.
func (m *Message) GetMessage(name string) (*Message, bool, error) {
	if err := m.check(); err != nil {
		return nil, false, err
	}
	f, err := m.typ.field(name)
	if err != nil {
		return nil, false, err
	}
	v := C.pbw_message_get_field(m.c, f)
	if v == nil {
		return nil, false, nil
	}
	return &Message{typ: m.typ.subtype(f), c: C.pbw_value_get_message(v), root: m.rootOrSelf()}, true, nil
}

func (m *Message) getSlice(name string) (*C.pbw_value, int, error) {
	v, err := m.getValue(name)
	if err != nil || v == nil {
		return nil, 0, err
	}
	return v, int(C.pbw_value_count(v)), nil
}

// GetIntSlice reads a repeated signed integer/enum field.
func (m *Message) GetIntSlice(name string) ([]int64, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]int64, n)
	for i := range out {
		out[i] = int64(C.pbw_value_get_int(C.pbw_value_at(v, C.size_t(i))))
	}
	return out, nil
}

// GetUintSlice reads a repeated unsigned integer field.
func (m *Message) GetUintSlice(name string) ([]uint64, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]uint64, n)
	for i := range out {
		out[i] = uint64(C.pbw_value_get_uint(C.pbw_value_at(v, C.size_t(i))))
	}
	return out, nil
}

// GetBoolSlice reads a repeated bool field.
func (m *Message) GetBoolSlice(name string) ([]bool, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]bool, n)
	for i := range out {
		out[i] = C.pbw_value_get_bool(C.pbw_value_at(v, C.size_t(i))) != 0
	}
	return out, nil
}

// GetFloatSlice reads a repeated float field.
func (m *Message) GetFloatSlice(name string) ([]float32, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]float32, n)
	for i := range out {
		out[i] = float32(C.pbw_value_get_float(C.pbw_value_at(v, C.size_t(i))))
	}
	return out, nil
}

// GetDoubleSlice reads a repeated double field.
func (m *Message) GetDoubleSlice(name string) ([]float64, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]float64, n)
	for i := range out {
		out[i] = float64(C.pbw_value_get_double(C.pbw_value_at(v, C.size_t(i))))
	}
	return out, nil
}

// GetStringSlice reads a repeated string field.
func (m *Message) GetStringSlice(name string) ([]string, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([]string, n)
	for i := range out {
		item := C.pbw_value_at(v, C.size_t(i))
		if C.pbw_value_get_kind(item) != C.PBW_VALUE_BYTES {
			return nil, fmt.Errorf("dynamic: field %q is not a string list", name)
		}
		var l C.size_t
		p := C.pbw_value_get_bytes(item, &l)
		b, err := copyBytes(unsafe.Pointer(p), l)
		if err != nil {
			return nil, err
		}
		out[i] = string(b)
	}
	return out, nil
}

// GetBytesSlice reads a repeated bytes field.
func (m *Message) GetBytesSlice(name string) ([][]byte, error) {
	v, n, err := m.getSlice(name)
	if err != nil || v == nil {
		return nil, err
	}
	out := make([][]byte, n)
	for i := range out {
		var l C.size_t
		item := C.pbw_value_at(v, C.size_t(i))
		p := C.pbw_value_get_bytes(item, &l)
		if C.pbw_value_get_kind(item) != C.PBW_VALUE_BYTES {
			return nil, fmt.Errorf("dynamic: field %q is not a bytes list", name)
		}
		b, err := copyBytes(unsafe.Pointer(p), l)
		if err != nil {
			return nil, err
		}
		out[i] = b
	}
	return out, nil
}

// GetMessageSlice reads a repeated sub-message field as views.
func (m *Message) GetMessageSlice(name string) ([]*Message, error) {
	if err := m.check(); err != nil {
		return nil, err
	}
	f, err := m.typ.field(name)
	if err != nil {
		return nil, err
	}
	v := C.pbw_message_get_field(m.c, f)
	if v == nil {
		return nil, nil
	}
	n := int(C.pbw_value_count(v))
	out := make([]*Message, n)
	for i := range out {
		sub := C.pbw_value_get_message(C.pbw_value_at(v, C.size_t(i)))
		out[i] = &Message{typ: m.typ.subtype(f), c: sub, root: m.rootOrSelf()}
	}
	return out, nil
}

// encodeShape picks which caller-buffer entry point to call: the core has
// one for each wire form, all with the same contract (no allocation, `cap`
// is the hard limit, PBW_ERR_LIMIT when it does not fit).
type encodeShape int

const (
	shapePlain encodeShape = iota
	shapeDeterministic
	shapeDelimited
)

// encodeTo writes the tree straight into Go memory. The C core used to
// malloc the result and hand ownership over, which cost a second downcall
// (pbw_free) plus a full copy back into Go for every encode; writing into a
// Go slice instead is one downcall and nothing to free. The capacity comes
// from the last successful encode (or the decoded wire length), so a hot loop
// reuses the size it already paid for and a cold message grows on demand.
func (m *Message) encodeTo(shape encodeShape, label string) ([]byte, error) {
	if err := m.check(); err != nil {
		return nil, err
	}
	cap := int(m.encCap.Load())
	if cap <= 0 {
		if n := C.pbw_message_wire_size(m.c); n > 0 {
			// Exact for a tree decoded and not mutated since; too small
			// after mutation, which the retry below covers.
			cap = int(n) + int(C.PBW_ENCODE_SLACK)
		} else {
			cap = 256 // the core's own small-message starting point
		}
	}
	for {
		out := make([]byte, cap)
		var l C.size_t
		var st C.pbw_status
		switch shape {
		case shapeDeterministic:
			st = C.pbw_encode_deterministic_to(m.c, unsafe.Pointer(&out[0]), C.size_t(cap), &l)
		case shapeDelimited:
			st = C.pbw_encode_delimited_to(m.c, unsafe.Pointer(&out[0]), C.size_t(cap), &l)
		default:
			st = C.pbw_encode_to(m.c, unsafe.Pointer(&out[0]), C.size_t(cap), &l)
		}
		runtime.KeepAlive(out)
		if st == C.PBW_OK {
			// Hint the exact size plus the writer's reservation slack, not
			// the capacity a retry happened to reach: a message that once
			// needed a doubling should not keep a 2x buffer.
			n := int(l)
			m.encCap.Store(int64(n) + int64(C.PBW_ENCODE_SLACK))
			if cap-n <= int(C.PBW_ENCODE_SLACK) {
				// At most a few bytes larger than the result: slicing the
				// array we already allocated is free, and the overshoot is
				// smaller than the copy it would replace.
				return out[:n], nil
			}
			// A retry left a much larger array than the result; copy into an
			// exact one so the caller's slice does not pin it.
			res := make([]byte, n)
			copy(res, out[:n])
			return res, nil
		}
		if st != C.PBW_ERR_LIMIT {
			return nil, statusErr(label, st)
		}
		if cap >= math.MaxInt32 {
			return nil, fmt.Errorf("dynamic: %s result exceeds the 2 GB limit", label)
		}
		cap *= 2
	}
}

// Encode serializes the tree into a fresh slice the caller owns.
func (m *Message) Encode() ([]byte, error) {
	return m.encodeTo(shapePlain, "dynamic: encode")
}

// EncodeDeterministic serializes with map entries sorted by key, so equal
// messages encode to equal bytes regardless of insertion order.
func (m *Message) EncodeDeterministic() ([]byte, error) {
	return m.encodeTo(shapeDeterministic, "dynamic: encode deterministic")
}

// EncodeDelimited serializes one length-delimited frame (varint length +
// message), the standard framing for a stream of messages.
func (m *Message) EncodeDelimited() ([]byte, error) {
	return m.encodeTo(shapeDelimited, "dynamic: encode delimited")
}

// DecodeDelimited decodes one length-delimited frame from the front of wire
// and reports how many bytes the frame occupied, so a caller holding
// concatenated frames advances by it and decodes again. Trailing bytes are
// left for the caller; a truncated frame fails.
func (t *MessageType) DecodeDelimited(wire []byte) (*Message, int, error) {
	cbuf := unsafe.Pointer(unsafe.SliceData(wire))
	var out *C.pbw_message
	var consumed C.size_t
	if st := C.pbw_decode_delimited(t.c, cbuf, C.size_t(len(wire)), &out, &consumed); st != C.PBW_OK {
		runtime.KeepAlive(wire)
		return nil, 0, statusErr("dynamic: decode delimited", st)
	}
	runtime.KeepAlive(wire)
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	return m, int(consumed), nil
}

// ToJSON prints the message in protobuf JSON mapping. Unknown fields are
// not printed, matching the reference printer.
func (m *Message) ToJSON() (string, error) {
	if err := m.check(); err != nil {
		return "", err
	}
	var out *C.char
	var l C.size_t
	if st := C.pbw_json_print(m.c, &out, &l); st != C.PBW_OK {
		return "", statusErr("dynamic: json print", st)
	}
	defer C.pbw_free(unsafe.Pointer(out))
	b, err := copyBytes(unsafe.Pointer(out), l)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

// ParseJSON parses JSON mapping text into a new tree. Unknown fields fail
// unless ignoreUnknown is set.
func (t *MessageType) ParseJSON(text string, ignoreUnknown bool) (*Message, error) {
	return t.ParseJSONLimits(text, ignoreUnknown, 0, 0)
}

// ParseJSONLimits is ParseJSON with explicit bounds: text longer than
// maxBytes fails, and nesting past maxDepth fails. Zero means the defaults.
func (t *MessageType) ParseJSONLimits(text string, ignoreUnknown bool, maxBytes uint64, maxDepth uint32) (*Message, error) {
	cstr := C.CString(text)
	defer C.free(unsafe.Pointer(cstr))
	var opts C.pbw_json_parse_options
	opts.ignore_unknown_fields = 0
	if ignoreUnknown {
		opts.ignore_unknown_fields = 1
	}
	var limits C.pbw_limits
	limits.max_bytes = C.size_t(maxBytes)
	limits.max_depth = C.uint32_t(maxDepth)
	var out *C.pbw_message
	if st := C.pbw_json_parse_limits(t.c, cstr, C.size_t(len(text)), &limits, &opts, &out); st != C.PBW_OK {
		return nil, statusErr("dynamic: json parse", st)
	}
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	return m, nil
}

// ToText prints the message in protobuf text format. Unknown fields print
// as numeric lines when printUnknown is set, like the reference printer.
func (m *Message) ToText(printUnknown bool) (string, error) {
	if err := m.check(); err != nil {
		return "", err
	}
	var out *C.char
	var l C.size_t
	flag := C.int(0)
	if printUnknown {
		flag = 1
	}
	if st := C.pbw_text_print_opts(m.c, flag, &out, &l); st != C.PBW_OK {
		return "", statusErr("dynamic: text print", st)
	}
	defer C.pbw_free(unsafe.Pointer(out))
	b, err := copyBytes(unsafe.Pointer(out), l)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

// ParseText parses text format into a new tree.
func (t *MessageType) ParseText(text string) (*Message, error) {
	return t.ParseTextLimits(text, 0, 0)
}

// ParseTextLimits is ParseText with explicit bounds. Zero means the defaults.
func (t *MessageType) ParseTextLimits(text string, maxBytes uint64, maxDepth uint32) (*Message, error) {
	cstr := C.CString(text)
	defer C.free(unsafe.Pointer(cstr))
	var limits C.pbw_limits
	limits.max_bytes = C.size_t(maxBytes)
	limits.max_depth = C.uint32_t(maxDepth)
	var out *C.pbw_message
	if st := C.pbw_text_parse_limits(t.c, cstr, C.size_t(len(text)), &limits, &out); st != C.PBW_OK {
		return nil, statusErr("dynamic: text parse", st)
	}
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	return m, nil
}

// DecodeLimits decodes with explicit bounds: inputs larger than maxBytes
// fail, and nesting past maxDepth fails. Zero means the defaults (no byte
// cap, PBW_MAX_DEPTH nesting).
func (t *MessageType) DecodeLimits(wire []byte, maxBytes uint64, maxDepth uint32) (*Message, error) {
	cbuf := unsafe.Pointer(unsafe.SliceData(wire))
	var out *C.pbw_message
	var limits C.pbw_limits
	limits.max_bytes = C.size_t(maxBytes)
	limits.max_depth = C.uint32_t(maxDepth)
	if st := C.pbw_decode_limits(t.c, cbuf, C.size_t(len(wire)), &limits, &out); st != C.PBW_OK {
		runtime.KeepAlive(wire)
		return nil, statusErr("dynamic: decode limits", st)
	}
	runtime.KeepAlive(wire)
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	return m, nil
}

// IsInitialized reports whether every proto2 required field is present
// (recursively). Always true for proto3 messages without proto2 submessages.
func (m *Message) IsInitialized() (bool, error) {
	if err := m.check(); err != nil {
		return false, err
	}
	return C.pbw_message_is_initialized(m.c) != 0, nil
}

// EncodedSize reports the exact number of bytes Encode writes, without
// writing them: one walk over the present values.
func (m *Message) EncodedSize() (uint64, error) {
	if err := m.check(); err != nil {
		return 0, err
	}
	var size C.size_t
	if st := C.pbw_message_encoded_size(m.c, &size); st != C.PBW_OK {
		return 0, statusErr("dynamic: encoded size", st)
	}
	return uint64(size), nil
}

// AnyTypeURL returns field 1 of a google.protobuf.Any tree, or false when
// absent. Owned by the tree: valid until the message closes.
func (m *Message) AnyTypeURL() (string, bool, error) {
	if err := m.check(); err != nil {
		return "", false, err
	}
	p := C.pbw_any_type_url(m.c)
	if p == nil {
		return "", false, nil
	}
	return C.GoString(p), true, nil
}

// AnyPayload returns field 2 of a google.protobuf.Any tree (copied out).
func (m *Message) AnyPayload() ([]byte, error) {
	if err := m.check(); err != nil {
		return nil, err
	}
	var l C.size_t
	p := C.pbw_any_payload(m.c, &l)
	if p == nil {
		return nil, nil
	}
	return copyBytes(unsafe.Pointer(p), l)
}

// TimestampMillis reads a google.protobuf.Timestamp tree as milliseconds.
func (m *Message) TimestampMillis() (int64, error) {
	if err := m.check(); err != nil {
		return 0, err
	}
	var ms C.int64_t
	if st := C.pbw_timestamp_to_millis(m.c, &ms); st != C.PBW_OK {
		return 0, statusErr("dynamic: timestamp millis", st)
	}
	return int64(ms), nil
}

// DurationMillis reads a google.protobuf.Duration tree as milliseconds.
func (m *Message) DurationMillis() (int64, error) {
	if err := m.check(); err != nil {
		return 0, err
	}
	var ms C.int64_t
	if st := C.pbw_duration_to_millis(m.c, &ms); st != C.PBW_OK {
		return 0, statusErr("dynamic: duration millis", st)
	}
	return int64(ms), nil
}

// FieldMaskPathCount returns how many paths a google.protobuf.FieldMask
// tree holds, without materializing the list.
func (m *Message) FieldMaskPathCount() (int, error) {
	if err := m.check(); err != nil {
		return 0, err
	}
	return int(C.pbw_fieldmask_path_count(m.c)), nil
}

// FieldMaskPathAt returns one FieldMask path (empty past the end).
func (m *Message) FieldMaskPathAt(i int) (string, error) {
	if err := m.check(); err != nil {
		return "", err
	}
	p := C.pbw_fieldmask_path_at(m.c, C.size_t(i))
	if p == nil {
		return "", nil
	}
	return C.GoString(p), nil
}

// ResolveMethod resolves service/method to its input/output message types
// for routing: decode with the first, encode with the second. The core does
// no RPC; a transport (gRPC, Twirp, plain HTTP) uses these.
func (s *Schema) ResolveMethod(service, method string) (*MessageType, *MessageType, error) {
	csvc := C.CString(service)
	defer C.free(unsafe.Pointer(csvc))
	cmeth := C.CString(method)
	defer C.free(unsafe.Pointer(cmeth))
	var inDesc, outDesc *C.pbw_descriptor
	if st := C.pbw_service_resolve(s.c, csvc, cmeth, &inDesc, &outDesc); st != C.PBW_OK {
		return nil, nil, statusErr("dynamic: resolve method", st)
	}
	s.retain()
	s.retain()
	return &MessageType{s: s, c: inDesc, fields: make(map[string]*C.pbw_field), own: true},
		&MessageType{s: s, c: outDesc, fields: make(map[string]*C.pbw_field), own: true}, nil
}

// VisitField is one scanned field: raw wire content, no allocation, no
// value tree. Name is empty when the schema does not declare the number.
// Raw carries the wire bits for varint (0) and the fixed widths (1, 5);
// Data carries the LEN payload (nested message, packed run, string) or the
// whole group field, and is nil for the scalar shapes. Data points into the
// caller's slice and is only valid for the duration of the callback.
type VisitField struct {
	Name     string
	Number   uint32
	WireType uint32
	Raw      uint64
	Data     []byte
}

// visitCall is the handle value the C trampoline reads back: the callback
// plus whether it asked to stop, so a stop is not mistaken for a failure.
type visitCall struct {
	cb      func(VisitField) bool
	stopped bool
}

// pbwgoVisitTramp is the C-callable trampoline; cgo does not accept C types
// in an exported signature, so it takes the ABI types directly.
//
//export pbwgoVisitTramp
func pbwgoVisitTramp(ud unsafe.Pointer, field unsafe.Pointer, number uint32, wireType uint32, value unsafe.Pointer) int32 {
	call := cgo.Handle(uintptr(ud)).Value().(*visitCall)
	ev := VisitField{Number: number, WireType: wireType}
	if field != nil {
		ev.Name = C.GoString(C.pbw_field_name((*C.pbw_field)(field)))
	}
	if value != nil {
		v := (*C.pbw_visit_value)(value)
		ev.Raw = uint64(v.u64)
		if v.data != nil {
			ev.Data = unsafe.Slice((*byte)(unsafe.Pointer(v.data)), int(v.len))
		}
	}
	if call.cb(ev) {
		return 0
	}
	call.stopped = true
	return 1
}

// Visit scans wire bytes one level deep without building a message tree: one
// validating tag pass, one callback per field, the core allocating nothing.
// Nested messages and packed runs arrive as Data payloads for the caller to
// recurse into. Returning false from cb stops the scan without an error.
// Zero bounds are the defaults.
func (t *MessageType) Visit(wire []byte, maxBytes uint64, maxDepth uint32, cb func(VisitField) bool) error {
	if cb == nil {
		return errors.New("dynamic: visit needs a callback")
	}
	call := &visitCall{cb: cb}
	h := cgo.NewHandle(call)
	defer h.Delete()
	var limits C.pbw_limits
	limits.max_bytes = C.size_t(maxBytes)
	limits.max_depth = C.uint32_t(maxDepth)
	var failOff C.size_t
	st := C.pbwgo_visit(t.c, unsafe.Pointer(unsafe.SliceData(wire)), C.size_t(len(wire)), &limits, C.uintptr_t(h), &failOff)
	runtime.KeepAlive(wire)
	if st != C.PBW_OK && !call.stopped {
		return statusErr("dynamic: visit", st)
	}
	return nil
}

// Decode parses wire bytes into a new root tree; zero-copy in, one downcall.
// The slice crosses into C for the call only: it holds no Go pointers and
// decode copies what it keeps, so no C-side input allocation happens.
func (t *MessageType) Decode(wire []byte) (*Message, error) {
	cbuf := unsafe.Pointer(unsafe.SliceData(wire))
	var out *C.pbw_message
	if st := C.pbw_decode(t.c, cbuf, C.size_t(len(wire)), &out); st != C.PBW_OK {
		runtime.KeepAlive(wire)
		return nil, statusErr("dynamic: decode", st)
	}
	runtime.KeepAlive(wire)
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	return m, nil
}

// Registry holds the current schema version and the retired ones.
// Load-then-Swap is the hot-swap: a failed load never touches the current
// version, and in-flight messages pin the schema they were built from.
type Registry struct {
	mu      sync.Mutex
	cur     atomic.Pointer[Schema]
	old     []*Schema
	version uint64
}

// NewRegistry loads the first version; it must succeed.
func NewRegistry(descSet []byte) (*Registry, error) {
	s, err := Load(descSet)
	if err != nil {
		return nil, err
	}
	s.version = 1
	r := &Registry{version: 1}
	r.cur.Store(s)
	return r, nil
}

// Current returns the live schema. The registry owns it; messages built
// from it retain their own reference, so swapping never invalidates them.
func (r *Registry) Current() *Schema { return r.cur.Load() }

// Swap loads descSet as the next version and makes it live; the previous
// version stays valid for in-flight messages until Purge.
func (r *Registry) Swap(descSet []byte) (*Schema, error) {
	s, err := Load(descSet)
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	prev := r.cur.Load()
	r.version++
	s.version = r.version
	r.old = append(r.old, prev)
	r.mu.Unlock()
	r.cur.Store(s)
	return s, nil
}

// Purge drops the registry's references to retired versions; versions with
// live messages free once those messages close.
func (r *Registry) Purge() {
	r.mu.Lock()
	old := r.old
	r.old = nil
	r.mu.Unlock()
	for _, s := range old {
		s.Close()
	}
}
