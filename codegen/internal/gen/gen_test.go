package gen

import "testing"

// A name derived from a file path must stay a usable identifier in the hosts
// that spell it as one (Python modules, Scala objects, JS locals).
func TestIdent(t *testing.T) {
	cases := map[string]string{
		"matrix3": "matrix3",
		"a-b":     "a_b",
		"a.b-c":   "a_b_c",
		"2fast":   "_2fast",
		"":        "_",
		"x_1":     "x_1",
	}
	for in, want := range cases {
		if got := ident(in); got != want {
			t.Errorf("ident(%q) = %q, want %q", in, got, want)
		}
	}
}
