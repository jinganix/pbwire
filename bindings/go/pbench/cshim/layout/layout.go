// Package layout is the DATA vocabulary the cshim codec and its generated
// tables share. It holds no wire logic and performs no I/O: Row describes
// one field's facts plus the struct layout the C codec reads through,
// Msg groups one message's rows, Arena stages decoded strings and bytes,
// ItabOf resolves oneof wrapper identities once at codec build time.
package layout

import "unsafe"

// Kind identifies the in-memory value shape and wire transform of one row,
// mirroring the CSHIMK_* enum in cshim_codec.h.
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

// Wire types on the wire. Values match the protobuf encoding.
type Wire uint32

const (
	WireVarint     Wire = 0
	WireFixed64    Wire = 1
	WireLen        Wire = 2
	WireStartGroup Wire = 3
	WireFixed32    Wire = 5
)

// Row is one field's layout facts. Offsets come from unsafe.Offsetof over
// the generated struct, evaluated once at init by generated code. Itab is
// the expected interface itab for a oneof member row, 0 otherwise; Ogrp
// is that member's staging slot for decode. Sub is the registry index of
// a message-typed field's message, -1 when the row holds no submessage.
// Foreign rows name their message by full proto name for the harness to
// resolve against a sibling registry.
type Row struct {
	Fnum     uint32
	Kind     Kind
	Repeated bool
	Packed   bool
	Wire     Wire
	Offset   uint32
	Has      uint32
	Explicit bool
	Required bool
	Group    bool
	Itab     uintptr
	Ogrp     uint32
	Sub      int32
	Foreign  bool
	FType    string
}

// Msg is one message's table: its full proto name, its struct size for
// the C arena, and its rows in field-number order.
type Msg struct {
	Full string
	Size uintptr
	Rows []Row
}

// IndexOf returns the registry index of the named message, or -1.
func IndexOf(reg []Msg, full string) int {
	for i, m := range reg {
		if m.Full == full {
			return i
		}
	}
	return -1
}

// ItabOf returns the interface itab word for a (possibly nil) pointer of
// a wrapper type, so the C codec can match a oneof member by pointer
// comparison with no Go work per call. Itabs are stable for the lifetime
// of the process.
func ItabOf(v any) uintptr {
	w := (*[2]unsafe.Pointer)(unsafe.Pointer(&v))
	return uintptr(w[0])
}

// Arena stages decoded string and byte payloads: one allocation per
// decode call when pre-sized, transparent growth otherwise. Earlier
// results alias arena memory and stay valid across later growth (growth
// moves the backing array; live strings keep the old one alive).
type Arena struct {
	buf []byte
}

// Reset prepares room for about n payload bytes, keeping nothing.
func (a *Arena) Reset(n int) {
	if n <= 0 {
		n = 1
	}
	a.buf = make([]byte, 0, n)
}

// AddString copies s into the arena without a second copy: the result
// aliases arena memory, which the decoded message owns after the fixup
// returns.
func (a *Arena) AddString(s string) string {
	if len(s) == 0 {
		return ""
	}
	start := len(a.buf)
	a.buf = append(a.buf, s...)
	return unsafe.String(&a.buf[start], len(s))
}

// AddBytes copies p into the arena.
func (a *Arena) AddBytes(p []byte) []byte {
	if len(p) == 0 {
		return nil
	}
	start := len(a.buf)
	a.buf = append(a.buf, p...)
	return a.buf[start:]
}
