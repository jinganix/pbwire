package pbwire

import (
	"bytes"
	"math"
	"testing"
)

func TestVarintRoundTrip(t *testing.T) {
	values := []uint64{0, 1, 127, 128, 255, 300, 16383, 16384, 1 << 31, 1 << 35,
		math.MaxUint32, 1 << 63, math.MaxUint64}
	for _, v := range values {
		b := AppendVarint(nil, v)
		if len(b) != VarintSize(v) {
			t.Fatalf("VarintSize(%d) = %d, encoded %d bytes", v, VarintSize(v), len(b))
		}
		got, n := ConsumeVarint(b)
		if n != len(b) || got != v {
			t.Fatalf("ConsumeVarint(%x) = %d, %d; want %d, %d", b, got, n, v, len(b))
		}
	}
}

func TestVarintTruncatedAndOverlong(t *testing.T) {
	if _, n := ConsumeVarint([]byte{0x80}); n >= 0 {
		t.Fatal("truncated varint accepted")
	}
	if _, n := ConsumeVarint(bytes.Repeat([]byte{0x80}, 11)); n >= 0 {
		t.Fatal("11-byte varint accepted")
	}
	// A 10th continuation byte is consumed like the reference does: the value
	// truncates to 64 bits instead of failing.
	got, n := ConsumeVarint([]byte{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f})
	if n != 10 || got != math.MaxUint64 {
		t.Fatalf("10-byte varint = %d, %d", got, n)
	}
}

func TestFinishSlice(t *testing.T) {
	for _, n := range []int{0, 1, 126, 127, 128, 129, 16383, 16384, 30000} {
		payload := bytes.Repeat([]byte{0xab}, n)
		got := FinishSlice(append([]byte{0}, payload...), 0)
		want := AppendBytes(nil, payload)
		if !bytes.Equal(got, want) {
			t.Fatalf("len %d: FinishSlice = %x, want %x", n, got, want)
		}
		// A mark away from zero must leave the prefix untouched.
		got = FinishSlice(append([]byte{0x08, 0}, payload...), 1)
		if !bytes.Equal(got[1:], want) || got[0] != 0x08 {
			t.Fatalf("len %d: FinishSlice at mark 1 = %x", n, got)
		}
	}
}

func TestFixedRoundTrip(t *testing.T) {
	f32 := AppendFixed32(nil, 0xdeadbeef)
	v32, n := ConsumeFixed32(f32)
	if n != 4 || v32 != 0xdeadbeef {
		t.Fatalf("fixed32 = %x, %d", v32, n)
	}
	f64 := AppendFixed64(nil, 0x0102030405060708)
	v64, n := ConsumeFixed64(f64)
	if n != 8 || v64 != 0x0102030405060708 {
		t.Fatalf("fixed64 = %x, %d", v64, n)
	}
	if _, n := ConsumeFixed32(f32[:3]); n >= 0 {
		t.Fatal("truncated fixed32 accepted")
	}
	if _, n := ConsumeFixed64(f64[:7]); n >= 0 {
		t.Fatal("truncated fixed64 accepted")
	}
}

func TestConsumeBytes(t *testing.T) {
	b := AppendBytes(nil, []byte("hello"))
	p, n := ConsumeBytes(b)
	if n != len(b) || string(p) != "hello" {
		t.Fatalf("ConsumeBytes = %q, %d", p, n)
	}
	if _, n := ConsumeBytes([]byte{0x05, 'h', 'i'}); n >= 0 {
		t.Fatal("truncated payload accepted")
	}
	if _, n := ConsumeBytes([]byte{0x80}); n >= 0 {
		t.Fatal("truncated length accepted")
	}
	if p, n := ConsumeBytes([]byte{0}); n != 1 || len(p) != 0 {
		t.Fatalf("empty payload = %q, %d", p, n)
	}
}

func TestConsumeTag(t *testing.T) {
	b := AppendVarint(nil, 19<<3|2)
	num, wt, n := ConsumeTag(b)
	if n != len(b) || num != 19 || wt != 2 {
		t.Fatalf("tag = %d, %d, %d", num, wt, n)
	}
	for _, bad := range [][]byte{
		{0x00},                               // field number 0
		{0x06},                               // wire type 6
		{0x07},                               // wire type 7
		{0x80},                               // truncated
		{0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}, // field number above 2^29
	} {
		if _, _, n := ConsumeTag(bad); n >= 0 {
			t.Fatalf("bad tag %x accepted", bad)
		}
	}
}

func TestSkipField(t *testing.T) {
	var b []byte
	b = AppendVarint(b, 1<<3|0)
	b = AppendVarint(b, 300)
	b = AppendVarint(b, 2<<3|5)
	b = AppendFixed32(b, 1)
	b = AppendVarint(b, 3<<3|1)
	b = AppendFixed64(b, 2)
	b = AppendVarint(b, 4<<3|2)
	b = AppendBytes(b, []byte("xx"))
	end := len(b)
	off := 0
	for off < end {
		num, wt, n := ConsumeTag(b[off:])
		if n < 0 {
			t.Fatalf("tag at %d", off)
		}
		off += n
		k := SkipField(b[off:], num, wt, 0)
		if k < 0 {
			t.Fatalf("skip field %d", num)
		}
		off += k
	}
	if off != end {
		t.Fatalf("skipped %d of %d bytes", off, end)
	}

	// SkipField receives the bytes after the start-group tag, like the decode
	// loop does after ConsumeTag.
	var group []byte
	group = AppendVarint(group, 1<<3|0)
	group = AppendVarint(group, 7)
	group = AppendVarint(group, 5<<3|4)
	if n := SkipField(group, 5, 3, MaxDepth-1); n != len(group) {
		t.Fatalf("group skip = %d, want %d", n, len(group))
	}
	if n := SkipField(group, 5, 3, MaxDepth); n >= 0 {
		t.Fatal("group skip at MaxDepth accepted")
	}
	if n := SkipField(group, 6, 3, 0); n >= 0 {
		t.Fatal("mismatched end group accepted")
	}
	if n := SkipField(group[:len(group)-1], 5, 3, 0); n >= 0 {
		t.Fatal("unterminated group accepted")
	}
	if n := SkipField([]byte{0x0c}, 1, 4, 0); n >= 0 {
		t.Fatal("stray end group accepted")
	}
}

func TestNestedGroupLimit(t *testing.T) {
	// Nested groups deeper than MaxDepth must fail without recursing further.
	var b []byte
	for i := 1; i <= MaxDepth; i++ {
		b = AppendVarint(b, uint64((i+1)<<3|3))
	}
	for i := MaxDepth; i >= 1; i-- {
		b = AppendVarint(b, uint64((i+1)<<3|4))
	}
	if n := SkipField(b, 1, 3, 0); n >= 0 {
		t.Fatal("deep group accepted")
	}
	if n := SkipField(b, 1, 3, MaxDepth); n >= 0 {
		t.Fatal("group at the limit accepted")
	}
}
