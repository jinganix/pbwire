package pbwire

import (
	"bytes"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	s := New()
	defer s.Close()
	if s.ABI() != 1 {
		t.Fatalf("ABI = %d, want 1", s.ABI())
	}
	msg, err := s.Message("Person", []Field{
		{Name: "name", Number: 1, Type: TString},
		{Name: "id", Number: 2, Type: TInt32},
	})
	if err != nil {
		t.Fatal(err)
	}
	wire, err := msg.Encode(map[uint32]Value{
		1: {Bytes: []byte("alice"), IsStr: true},
		2: {Uint: 1},
	})
	if err != nil {
		t.Fatal(err)
	}
	got, err := msg.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got[1].Bytes, []byte("alice")) || got[2].Uint != 1 {
		t.Fatalf("round trip = %v", got)
	}
	// Absent fields stay absent; empty input decodes to no fields.
	empty, err := msg.Decode(nil)
	if err != nil || len(empty) != 0 {
		t.Fatalf("empty decode = %v, %v", empty, err)
	}
	if _, err := msg.Decode(wire[:len(wire)-1]); err == nil {
		t.Fatal("truncated decode should fail")
	}
}
