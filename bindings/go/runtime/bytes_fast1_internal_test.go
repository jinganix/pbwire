// Unit pin for bytesFast1: the 1-byte length fast path must accept exactly
// the inputs Decoder.Bytes accepts in its first branch (same bytes, same
// Off movement), and miss everything else so the caller falls back to
// Bytes and its errors. Fails to build before the helper exists.
package runtime

import (
	"bytes"
	"testing"
)

func TestBytesFast1(t *testing.T) {
	// 1-byte hit: length 3, payload + trailer.
	d := &Decoder{Buf: []byte{0x03, 'a', 'b', 'c', 0xFF}}
	raw, ok := d.bytesFast1()
	if !ok || !bytes.Equal(raw, []byte{'a', 'b', 'c'}) || d.Off != 4 {
		t.Fatalf("hit = %q,%v,off=%d; want abc,true,4", raw, ok, d.Off)
	}
	// Zero length hits with an empty slice.
	d = &Decoder{Buf: []byte{0x00}}
	raw, ok = d.bytesFast1()
	if !ok || len(raw) != 0 || d.Off != 1 {
		t.Fatalf("zero = %q,%v,off=%d; want empty,true,1", raw, ok, d.Off)
	}
	// 2-byte length misses without consuming.
	payload := bytes.Repeat([]byte{'x'}, 200)
	wire := append([]byte{0xC8, 0x01}, payload...)
	d = &Decoder{Buf: wire}
	if raw, ok := d.bytesFast1(); ok || raw != nil || d.Off != 0 {
		t.Fatalf("multibyte = %v,%v,off=%d; want miss with off=0", raw, ok, d.Off)
	}
	// Truncated 1-byte form misses (Bytes reports ErrWire for it).
	d = &Decoder{Buf: []byte{0x05, 'a'}}
	if _, ok := d.bytesFast1(); ok || d.Off != 0 {
		t.Fatalf("truncated hit, off=%d; want miss with off=0", d.Off)
	}
	full, err := d.Bytes()
	if err == nil || len(full) != 0 {
		t.Fatalf("truncated Bytes = %q,%v; want ErrWire", full, err)
	}
	// Empty input misses (Bytes reports ErrWire for it too).
	d = &Decoder{}
	if _, ok := d.bytesFast1(); ok {
		t.Fatal("empty hit; want miss")
	}
}
