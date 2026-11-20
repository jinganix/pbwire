package layout

import "testing"

type wrapA struct{ N int32 }

func (*wrapA) isK() {}

type wrapB struct{ N int32 }

func (*wrapB) isK() {}

func TestItabOf(t *testing.T) {
	a := ItabOf((*wrapA)(nil))
	b := ItabOf((*wrapB)(nil))
	if a == 0 || b == 0 {
		t.Fatal("itab of a named pointer type must be non-zero")
	}
	if a == b {
		t.Fatal("distinct wrapper types must have distinct itabs")
	}
	// Stable within the process: the C codec compares against these.
	if ItabOf((*wrapA)(nil)) != a {
		t.Fatal("itab must be stable across calls")
	}
}

func TestArena(t *testing.T) {
	var a Arena
	a.Reset(11)
	s := a.AddString("hello")
	p := a.AddBytes([]byte("world"))
	if s != "hello" || string(p) != "world" {
		t.Fatalf("arena round trip: %q %q", s, p)
	}
	if a.AddString("") != "" || a.AddBytes(nil) != nil {
		t.Fatal("empty payloads stay empty")
	}
	// Copies own their memory: mutating the input must not alias.
	src := []byte("abc")
	c := a.AddBytes(src)
	src[0] = 'X'
	if string(c) != "abc" {
		t.Fatal("arena aliases input")
	}
}
