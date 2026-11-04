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
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime"
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

// Message is one dynamic tree. Roots own their tree (Close frees it);
// nested views from NewMessage/GetMessage read into the root and must not
// be freed separately.
type Message struct {
	typ  *MessageType
	c    *C.pbw_message
	root *Message // non-nil for views: keeps the root (and schema) alive
	own  bool
}

// New builds an empty root message.
func (t *MessageType) New() *Message {
	m := &Message{typ: t, c: C.pbw_message_new(t.c), own: true}
	t.s.retain()
	runtime.SetFinalizer(m, func(m *Message) { m.Close() })
	return m
}

// Close frees a root tree; idempotent, a no-op for views.
func (m *Message) Close() {
	if m == nil || !m.own || m.c == nil {
		return
	}
	h := m.c
	m.c = nil
	m.typ.s.release()
	C.pbw_message_free(h)
}

func (m *Message) store(f *C.pbw_field, v *C.pbw_value) error {
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

// NewMessage creates (or returns the existing) nested message for a MESSAGE
// field: singular replaces, repeated appends. The view writes straight into
// the root tree; never Close it separately.
func (m *Message) NewMessage(name string) (*Message, error) {
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

// Encode serializes the tree; one cgo downcall plus one heap copy out.
func (m *Message) Encode() ([]byte, error) {
	var out *C.uchar
	var l C.size_t
	if st := C.pbw_encode(m.c, &out, &l); st != C.PBW_OK {
		return nil, statusErr("dynamic: encode", st)
	}
	defer C.pbw_free(unsafe.Pointer(out))
	return C.GoBytes(unsafe.Pointer(out), C.int(l)), nil
}

// Decode parses wire bytes into a new root tree; one copy in, one downcall.
func (t *MessageType) Decode(wire []byte) (*Message, error) {
	var cbuf unsafe.Pointer
	if len(wire) > 0 {
		cbuf = C.CBytes(wire)
		defer C.free(cbuf)
	}
	var out *C.pbw_message
	if st := C.pbw_decode(t.c, cbuf, C.size_t(len(wire)), &out); st != C.PBW_OK {
		return nil, statusErr("dynamic: decode", st)
	}
	m := &Message{typ: t, c: out, own: true}
	t.s.retain()
	runtime.SetFinalizer(m, func(m *Message) { m.Close() })
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
