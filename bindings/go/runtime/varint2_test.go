package runtime

import "testing"

// Two-byte varint fast path shared by Decoder.Varint and Decoder.Next:
// values 128..16383 are the packed/small-int bulk (Nums, Inner values,
// two-byte tags). One boundary load instead of the counting loop.
func TestConsumeVarint2(t *testing.T) {
	for _, c := range []struct {
		in   []byte
		want uint64
	}{
		{[]byte{0x80, 0x00}, 0}, // non-minimal: slow loop accepts it as 0
		{[]byte{0xAC, 0x02}, 300},
		{[]byte{0xFF, 0x7F}, 16383},
		{[]byte{0x80, 0x01}, 128},
	} {
		v, n, ok := consumeVarint2(c.in)
		if !ok || n != 2 || v != c.want {
			t.Errorf("consumeVarint2(%x) = %d,%d,%v, want %d,2,true", c.in, v, n, ok, c.want)
		}
	}
	for _, in := range [][]byte{
		{0x05},       // one byte: the existing single-byte path owns it
		{},           // empty
		{0x80},       // truncated
		{0x80, 0x80}, // continues past two bytes: slow loop owns it
		{0x05, 0x00}, // first byte terminates: slow loop owns it
	} {
		if _, _, ok := consumeVarint2(in); ok {
			t.Errorf("consumeVarint2(%x) = ok, want fallback", in)
		}
	}
}
