// Package codec is the pbench pure-Go codec candidate for generated struct
// models: the schema-driven engine the Go emitter programs against, grown out
// of the V3 probe. Wire primitives live here, the descriptor-set plan in
// plan.go, and the Encoder/Decoder driver in codec.go. Models the shipped
// cshim runtime cannot drive fall back to it, so the generated metadata file
// imports this package.
//
// The codec never knows any Go struct layout. Generated Marshal/Unmarshal
// code is straight-line field code that pushes values through Encoder and
// pulls them out of Decoder. No reflection runs in the hot path; the only
// reflection in the package lives in plan.go, which runs once at Load.
package codec

import "errors"

// WireType is a protobuf wire type. Values 0-5 are valid on the wire;
// anything else is rejected at decode time.
type WireType uint8

const (
	WireVarint     WireType = 0
	WireFixed64    WireType = 1
	WireLen        WireType = 2
	WireStartGroup WireType = 3
	WireEndGroup   WireType = 4
	WireFixed32    WireType = 5
)

// MaxDepth caps nested message/group recursion on decode (and encode).
// It matches the V3 probe: deep enough for any real schema, shallow enough
// that a depth bomb cannot exhaust the goroutine stack.
const MaxDepth = 100

// MaxFieldNumber is the largest legal field number (2^29 - 1).
const MaxFieldNumber = 1<<29 - 1

var (
	// ErrWire reports malformed wire data: truncation, bad varints, field
	// number 0, wire types 6/7, length overruns, or a mismatched group end.
	ErrWire = errors.New("codec: malformed wire data")
	// ErrDepth reports nesting past MaxDepth.
	ErrDepth = errors.New("codec: nesting exceeds max depth")
)

// CheckDepth rejects depths past MaxDepth. Generated nested decode calls it
// on entry with the depth it was invoked at.
func CheckDepth(depth int) error {
	if depth > MaxDepth {
		return ErrDepth
	}
	return nil
}

// SizeVarint is the encoded length of v as a varint.
func SizeVarint(v uint64) int {
	n := 1
	for v >= 0x80 {
		n++
		v >>= 7
	}
	return n
}

// SizeTag is the encoded length of a field tag.
func SizeTag(num uint32, wt WireType) int {
	return SizeVarint(uint64(num)<<3 | uint64(wt))
}

// AppendVarint appends v in varint encoding. It never fails; the caller
// owns the returned slice.
func AppendVarint(b []byte, v uint64) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

// AppendTag appends the tag for field num with wire type wt.
func AppendTag(b []byte, num uint32, wt WireType) []byte {
	return AppendVarint(b, uint64(num)<<3|uint64(wt))
}

// AppendFixed32 appends v little-endian.
func AppendFixed32(b []byte, v uint32) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}

// AppendFixed64 appends v little-endian.
func AppendFixed64(b []byte, v uint64) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24),
		byte(v>>32), byte(v>>40), byte(v>>48), byte(v>>56))
}

// AppendLen appends a length-delimited payload: varint length plus bytes.
func AppendLen(b, p []byte) []byte {
	b = AppendVarint(b, uint64(len(p)))
	return append(b, p...)
}

// ConsumeVarint reads one varint at the front of b, returning the value and
// the bytes consumed. It rejects empty input, truncation, overlong (>10
// byte) encodings, and a 10th byte that keeps the continuation bit set.
func ConsumeVarint(b []byte) (uint64, int, error) {
	var v uint64
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
			return v, i + 1, nil
		}
		v |= uint64(c&0x7f) << uint(7*i)
		if c < 0x80 {
			return v, i + 1, nil
		}
	}
	return 0, 0, ErrWire
}

// ConsumeTag reads one tag, rejecting field number 0, numbers past
// MaxFieldNumber, and wire types above 5 (6 and 7 included).
func ConsumeTag(b []byte) (uint32, WireType, int, error) {
	v, n, err := ConsumeVarint(b)
	if err != nil {
		return 0, 0, 0, err
	}
	num := uint32(v >> 3)
	wt := WireType(v & 7)
	if num == 0 || v>>3 > MaxFieldNumber || wt > WireFixed32 {
		return 0, 0, 0, ErrWire
	}
	return num, wt, n, nil
}

// ConsumeFixed32 reads 4 little-endian bytes.
func ConsumeFixed32(b []byte) (uint32, int, error) {
	if len(b) < 4 {
		return 0, 0, ErrWire
	}
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24, 4, nil
}

// ConsumeFixed64 reads 8 little-endian bytes.
func ConsumeFixed64(b []byte) (uint64, int, error) {
	if len(b) < 8 {
		return 0, 0, ErrWire
	}
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56, 8, nil
}

// ConsumeLen reads a length-delimited payload, returning a sub-slice that
// aliases b. The caller copies it if the input may be reused or mutated.
func ConsumeLen(b []byte) ([]byte, int, error) {
	n, m, err := ConsumeVarint(b)
	if err != nil {
		return nil, 0, err
	}
	if n > uint64(len(b)-m) {
		return nil, 0, ErrWire
	}
	end := m + int(n)
	return b[m:end], end, nil
}

// CountVarints counts the packed varints in b so generated decode can
// pre-size the destination slice with one pass. It rejects malformed input.
func CountVarints(b []byte) (int, error) {
	n := 0
	for len(b) > 0 {
		_, m, err := ConsumeVarint(b)
		if err != nil {
			return 0, err
		}
		b = b[m:]
		n++
	}
	return n, nil
}

// ZigZagEncode32 maps a signed 32-bit value to its varint form.
func ZigZagEncode32(v int32) uint64 {
	return uint64(uint32(v)<<1 ^ uint32(v>>31))
}

// ZigZagDecode32 maps a varint back to int32.
func ZigZagDecode32(v uint64) int32 {
	return int32(uint32(v)>>1) ^ -int32(v&1)
}

// ZigZagEncode64 maps a signed 64-bit value to its varint form.
func ZigZagEncode64(v int64) uint64 {
	return uint64(v)<<1 ^ uint64(v>>63)
}

// ZigZagDecode64 maps a varint back to int64.
func ZigZagDecode64(v uint64) int64 {
	return int64(v>>1) ^ -int64(v&1)
}
