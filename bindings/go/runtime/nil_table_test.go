// Nil tables must fail, never panic: a table lookup that errored (or was
// never built) surfaces here as nil, and the project rule is errors out,
// panics never. Fails before the entry guards exist (panic); passes after.
package runtime_test

import (
	"testing"

	pbwire "pbwire/runtime"
)

func TestNilTableFails(t *testing.T) {
	msg := &fallbackKid{}
	if _, err := pbwire.EncodeModel(msg, nil); err == nil {
		t.Fatal("EncodeModel with nil table succeeded, want error")
	}
	if err := pbwire.DecodeModel([]byte{0x08, 0x01}, msg, nil); err == nil {
		t.Fatal("DecodeModel with nil table succeeded, want error")
	}
	d := &pbwire.Decoder{Buf: []byte{0x08, 0x01}}
	a := &pbwire.Arena{}
	if err := pbwire.DecodeTableLimits(d, a, msg, nil, 0, pbwire.Limits{}); err == nil {
		t.Fatal("DecodeTableLimits with nil table succeeded, want error")
	}
}
