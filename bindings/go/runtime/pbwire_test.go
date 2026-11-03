package pbwire

import (
	"bytes"
	"path/filepath"
	"runtime"
	"testing"
)

var descPath = filepath.Join(sourceDir(), "..", "..", "c", "tests", "data", "matrix3.desc")

func TestLoadAndRoundTrip(t *testing.T) {
	s, err := LoadFile(descPath)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if s.ABI() != 1 {
		t.Fatalf("ABI = %d, want 1", s.ABI())
	}

	msg, err := s.Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	if len(msg.fields) != 22 {
		t.Fatalf("fields = %d, want 22", len(msg.fields))
	}

	wire, err := msg.Encode(map[uint32]Value{
		1:  {Uint: 42},                            // i32
		2:  {Uint: uint64(1) << 62},               // i64
		14: {Bytes: []byte("hello"), IsStr: true}, // str
	})
	if err != nil {
		t.Fatal(err)
	}
	got, err := msg.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	if got[1].Uint != 42 || got[2].Uint != uint64(1)<<62 ||
		!bytes.Equal(got[14].Bytes, []byte("hello")) {
		t.Fatalf("round trip = %+v", got)
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

func TestUnknownMessage(t *testing.T) {
	s, err := LoadFile(descPath)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if _, err := s.Message("nope.Nope"); err == nil {
		t.Fatal("unknown message should fail")
	}
}

func TestBadDescriptorSet(t *testing.T) {
	if _, err := Load([]byte{0xff, 0xff}); err == nil {
		t.Fatal("garbage descriptor set should fail")
	}
	if _, err := Load(nil); err == nil {
		t.Fatal("empty descriptor set should fail")
	}
}

// sourceDir is this file's directory, so the descriptor path does not depend on
// the working directory Go picks for the package.
func sourceDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(file)
}
