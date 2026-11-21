// The driver the generated code programs against: Encoder for Marshal,
// Decoder for Unmarshal, plus the Arena for string storage.
//
// Ownership rules, stated once:
//   - Encoder writes into Buf. A zero Encoder is ready; pre-size Buf when
//     the message size is roughly known. Result hands the buffer to the
//     caller: with a fresh Encoder per Marshal the result is a fresh
//     caller-owned slice. Reuse an Encoder via Reset only after the
//     previous Result is no longer needed; Reset keeps the capacity.
//   - Decoder borrows the caller's wire slice: it never copies except via
//     String. Bytes returns a sub-slice aliasing the input, so generated
//     code must copy (string() copies; byte fields go through Arena or
//     append([]byte(nil), ...)) before the input may be reused or mutated.
//   - Arena is a bump allocator owned by one decode call. Decoded strings
//     and byte copies point into it; the message owns the arena's memory
//     once Unmarshal returns, independent of the input slice. Arenas are
//     not safe for concurrent use; one per decode call.
//   - Errors are the sentinels ErrWire and ErrDepth from wire.go.
//     Generated decode returns them directly.
//
// Group pattern (proto2 groups; the plan marks them with Group=true):
//
//	enc.Tag(n, codec.WireStartGroup)
//	... member fields ...
//	enc.Tag(n, codec.WireEndGroup)
//
//	for {
//	    num, wt, err := d.Next()
//	    if err != nil { return err }
//	    if wt == codec.WireEndGroup {
//	        if num != n { return codec.ErrWire }
//	        break
//	    }
//	    ... member fields, d.Skip(num, wt, depth) by default ...
//	}
//
// Packed repeated numerics encode as one length-delimited run and decode
// accepting both packed runs and unpacked singles, like protobuf-go.
// Unknown fields (including extensions, which the emitter never generates
// accessors for) decode via Skip and are dropped, as on the dynamic path.
package runtime

import "unsafe"

// Encoder is a reusable append-only buffer for straight-line Marshal code.
// The zero value is ready; Buf may be pre-sized. Not safe for concurrent
// use; one Encoder per in-flight Marshal.
type Encoder struct {
	Buf []byte
}

// Reset drops the contents, keeping the capacity for the next Marshal.
// Call it only after the previous Result is no longer needed: Result
// aliases the same backing array.
func (e *Encoder) Reset() {
	e.Buf = e.Buf[:0]
}

// Result hands the accumulated bytes to the caller. With a fresh Encoder
// the slice is fresh and caller-owned.
func (e *Encoder) Result() []byte {
	return e.Buf
}

// Tag appends the tag for field num with wire type wt.
func (e *Encoder) Tag(num uint32, wt WireType) {
	e.Buf = AppendTag(e.Buf, num, wt)
}

// Varint appends v in varint encoding.
func (e *Encoder) Varint(v uint64) {
	e.Buf = AppendVarint(e.Buf, v)
}

// Fixed32 appends v little-endian.
func (e *Encoder) Fixed32(v uint32) {
	e.Buf = AppendFixed32(e.Buf, v)
}

// Fixed64 appends v little-endian.
func (e *Encoder) Fixed64(v uint64) {
	e.Buf = AppendFixed64(e.Buf, v)
}

// Data appends a length-delimited payload.
func (e *Encoder) Data(p []byte) {
	e.Buf = AppendLen(e.Buf, p)
}

// Text appends a length-delimited string.
func (e *Encoder) Text(s string) {
	e.Buf = AppendVarint(e.Buf, uint64(len(s)))
	e.Buf = append(e.Buf, s...)
}

// Begin starts a length-delimited field (submessage or packed run): it
// writes the tag plus a one-byte length placeholder and returns a mark for
// End. Marks nest but must close LIFO: End the most recent Begin first.
func (e *Encoder) Begin(num uint32) int {
	e.Tag(num, WireLen)
	mark := len(e.Buf)
	e.Buf = append(e.Buf, 0)
	return mark
}

// End patches the length opened by Begin. The payload between mark and the
// buffer end shifts right when the length itself needs more than one byte,
// so no Begin opened after mark may still be open.
func (e *Encoder) End(mark int) {
	n := len(e.Buf) - mark - 1
	if n < 0x80 {
		e.Buf[mark] = byte(n)
		return
	}
	var tmp [10]byte
	v := uint64(n)
	i := 0
	for v >= 0x80 {
		tmp[i] = byte(v) | 0x80
		v >>= 7
		i++
	}
	tmp[i] = byte(v)
	i++
	e.Buf = append(e.Buf, tmp[1:i]...)
	copy(e.Buf[mark+i:], e.Buf[mark+1:])
	copy(e.Buf[mark:], tmp[:i])
}

// Decoder is a bounds-checked cursor over a caller's wire slice for
// straight-line Unmarshal code. It borrows Buf and never retains it beyond
// the values the caller copies out. Not safe for concurrent use.
type Decoder struct {
	Buf []byte
	Off int
	// lastTag is where the tag of the current field started, so SkipRaw can
	// hand back the field bytes with their tag: a hook that retains unknown
	// fields needs re-emittable bytes, and a tag-less remainder is not.
	lastTag int
}

// Done reports whether the cursor reached the end of the input.
func (d *Decoder) Done() bool {
	return d.Off >= len(d.Buf)
}

// Rest returns the unread remainder. It aliases the input.
func (d *Decoder) Rest() []byte {
	return d.Buf[d.Off:]
}

// Next reads the next tag, returning its field number and wire type. It
// rejects field number 0 and wire types above 5. The varint loop is inline
// (same accept set as ConsumeVarint/ConsumeTag): Next runs once per field
// occurrence, so one call level here is one per field on every decode.
func (d *Decoder) Next() (uint32, WireType, error) {
	d.lastTag = d.Off
	b := d.Buf[d.Off:]
	var v uint64
	n := 0
	for i := 0; i < 10; i++ {
		if i >= len(b) {
			return 0, 0, ErrWire
		}
		c := b[i]
		if i == 9 {
			if c >= 0x80 {
				return 0, 0, ErrWire
			}
			v |= uint64(c) << 63
			n = 10
			break
		}
		v |= uint64(c&0x7f) << uint(7*i)
		if c < 0x80 {
			n = i + 1
			break
		}
	}
	num := uint32(v >> 3)
	wt := WireType(v & 7)
	if num == 0 || v>>3 > MaxFieldNumber || wt > WireFixed32 {
		return 0, 0, ErrWire
	}
	d.Off += n
	return num, wt, nil
}

// SkipRaw discards one field value and returns its bytes including the tag
// (the whole start/end frame for a group). The slice aliases the input, so a
// caller that keeps it must copy.
func (d *Decoder) SkipRaw(num uint32, wt WireType, depth int) ([]byte, error) {
	start := d.lastTag
	if err := d.Skip(num, wt, depth); err != nil {
		return nil, err
	}
	return d.Buf[start:d.Off], nil
}

// Varint reads one varint. The loop is inline (same accept set as
// ConsumeVarint): scalar fields parse one varint each, so one call level
// here is one per scalar on every decode.
func (d *Decoder) Varint() (uint64, error) {
	b := d.Buf[d.Off:]
	var v uint64
	for i := 0; i < 10; i++ {
		if i >= len(b) {
			return 0, ErrWire
		}
		c := b[i]
		if i == 9 {
			if c >= 0x80 {
				return 0, ErrWire
			}
			d.Off += 10
			return v | uint64(c)<<63, nil
		}
		v |= uint64(c&0x7f) << uint(7*i)
		if c < 0x80 {
			d.Off += i + 1
			return v, nil
		}
	}
	return 0, ErrWire
}

// Fixed32 reads 4 little-endian bytes.
func (d *Decoder) Fixed32() (uint32, error) {
	b := d.Buf[d.Off:]
	if len(b) < 4 {
		return 0, ErrWire
	}
	d.Off += 4
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24, nil
}

// Fixed64 reads 8 little-endian bytes.
func (d *Decoder) Fixed64() (uint64, error) {
	b := d.Buf[d.Off:]
	if len(b) < 8 {
		return 0, ErrWire
	}
	d.Off += 8
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56, nil
}

// Bytes reads a length-delimited payload as a sub-slice aliasing the input.
// Copy it (Arena.AddBytes or append) before the input may change. The length
// loop is inline (same accept set as ConsumeLen): strings, bytes, packed
// runs and submessages each parse one length per occurrence.
func (d *Decoder) Bytes() ([]byte, error) {
	b := d.Buf[d.Off:]
	var n uint64
	m := 0
	for i := 0; i < 10; i++ {
		if i >= len(b) {
			return nil, ErrWire
		}
		c := b[i]
		if i == 9 {
			if c >= 0x80 {
				return nil, ErrWire
			}
			n |= uint64(c) << 63
			m = 10
			break
		}
		n |= uint64(c&0x7f) << uint(7*i)
		if c < 0x80 {
			m = i + 1
			break
		}
	}
	if n > uint64(len(b)-m) {
		return nil, ErrWire
	}
	end := m + int(n)
	d.Off += end
	return b[m:end], nil
}

// String reads a length-delimited payload as a string. The conversion
// copies, so the result is independent of the input. No UTF-8 validation
// is done, matching protobuf-go unmarshal.
func (d *Decoder) String() (string, error) {
	p, err := d.Bytes()
	if err != nil {
		return "", err
	}
	return string(p), nil
}

// Skip discards one field value of the given wire type. Groups recurse with
// depth+1 and must close with the matching end tag; an end tag at top level
// (or a mismatched one) is ErrWire.
func (d *Decoder) Skip(num uint32, wt WireType, depth int) error {
	switch wt {
	case WireVarint:
		_, err := d.Varint()
		return err
	case WireFixed64:
		if len(d.Buf)-d.Off < 8 {
			return ErrWire
		}
		d.Off += 8
		return nil
	case WireLen:
		_, err := d.Bytes()
		return err
	case WireStartGroup:
		if err := CheckDepth(depth + 1); err != nil {
			return err
		}
		for {
			inner, innerWT, err := d.Next()
			if err != nil {
				return err
			}
			if innerWT == WireEndGroup {
				if inner != num {
					return ErrWire
				}
				return nil
			}
			if err := d.Skip(inner, innerWT, depth+1); err != nil {
				return err
			}
		}
	case WireFixed32:
		if len(d.Buf)-d.Off < 4 {
			return ErrWire
		}
		d.Off += 4
		return nil
	default:
		return ErrWire // end group at top level, or impossible 6/7
	}
}

// Arena is a bump allocator for one decode call's string and byte storage.
// Grow once with the input length (an upper bound: every stored byte comes
// from the input), then AddString/AddBytes copy payloads in without further
// allocation. The zero value is ready; Grow is optional but avoids growth.
type Arena struct {
	buf []byte
}

// Grow pre-allocates room for about n payload bytes.
func (a *Arena) Grow(n int) {
	if n <= 0 || cap(a.buf)-len(a.buf) >= n {
		return
	}
	next := make([]byte, len(a.buf), len(a.buf)+n)
	copy(next, a.buf)
	a.buf = next
}

// Reset drops the contents, keeping the capacity for the next call.
func (a *Arena) Reset() {
	a.buf = a.buf[:0]
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

// AddString copies p into the arena as a string without a second copy:
// the result aliases arena memory, which the decoded message owns after
// Unmarshal returns. It stays valid across later arena growth (growth
// moves the backing array; live strings keep the old one alive).
func (a *Arena) AddString(p []byte) string {
	if len(p) == 0 {
		return ""
	}
	start := len(a.buf)
	a.buf = append(a.buf, p...)
	return unsafe.String(&a.buf[start], len(p))
}
