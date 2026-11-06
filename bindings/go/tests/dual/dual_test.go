// Dual registration for the Go binding: the generated models encode/decode
// through the embedded descriptor set by default, and through an externally
// loaded one after UseSchema. A nil call restores the embedded set.
//
// Run: go test ./... (from bindings/go; needs build/libpbwire.a, like
// the go-test target). This package is separate from tests/ so the global
// override never leaks into the compat suite.
package dual

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"pbwire/dynamic"
	genone "pbwire/tests/gen/oneof1"
)

func testdata(t *testing.T, rel string) string {
	t.Helper()
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "..", "..", "codegen", "testdata", rel)
}

// External registration: models follow a dynamic.Load schema, and a nil
// call restores the embedded set afterwards.
func TestUseSchemaExternal(t *testing.T) {
	raw, err := os.ReadFile(testdata(t, "oneof1.desc"))
	if err != nil {
		t.Fatal(err)
	}
	ext, err := dynamic.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	defer ext.Close()
	genone.UseSchema(ext)
	defer genone.UseSchema(nil)

	ref, err := os.ReadFile(testdata(t, "oneof1.bin"))
	if err != nil {
		t.Fatal(err)
	}
	got := &genone.Oneof1{}
	if err := got.Unmarshal(ref); err != nil {
		t.Fatal(err)
	}
	if got.Name != "a" {
		t.Fatalf("name through external schema: %q", got.Name)
	}
	txt, ok := got.Kind.(*genone.Oneof1_Text)
	if !ok || txt.Text != "hi" {
		t.Fatalf("oneof member through external schema: %#v", got.Kind)
	}
	if wire, err := got.Marshal(); err != nil || string(wire) != string(ref) {
		t.Fatal("re-encode through external schema is not byte exact")
	}

	// Back on embedded: same bytes, no schema argument anywhere.
	got2 := &genone.Oneof1{}
	if err := got2.Unmarshal(ref); err != nil {
		t.Fatal(err)
	}
	if got2.Name != "a" {
		t.Fatalf("name after restore: %q", got2.Name)
	}
}

// Preload warms every generated type: after it, encode pays no lookup.
func TestPreload(t *testing.T) {
	if err := genone.Preload(); err != nil {
		t.Fatal(err)
	}
	one := &genone.Oneof1{Name: "a", Kind: &genone.Oneof1_Text{Text: "hi"}}
	wire, err := one.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	back := &genone.Oneof1{}
	if err := back.Unmarshal(wire); err != nil {
		t.Fatal(err)
	}
	if back.Name != "a" {
		t.Fatalf("round trip after preload: %q", back.Name)
	}
	if len(genone.MessageNames) == 0 {
		t.Fatal("empty message list")
	}
}
