package tests

// Cross-file coverage for the generated model: UsesDep references DepMsg
// from xdep.proto, so the xfile model imports the sibling xdep package
// (M flag, protoc-gen-go style) instead of skipping the field. Reference
// bytes are codegen/testdata/xfile.bin.
import (
	"bytes"
	"os"
	"testing"

	genxdep "pbwire/tests/gen/xdep"
	genxfile "pbwire/tests/gen/xfile"
)

func TestXfileBytes(t *testing.T) {
	ref, err := os.ReadFile(testdata(t, "xfile.bin"))
	if err != nil {
		t.Fatal(err)
	}
	msg := &genxfile.UsesDep{
		Single: &genxdep.DepMsg{Name: "one", Value: 7},
		Many: []*genxdep.DepMsg{
			{Name: "a"},
			{Name: "b", Value: -1},
		},
		Note: "hi",
	}
	wire, err := msg.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, ref) {
		t.Fatal("generated xfile Marshal != protoc reference bytes")
	}
	back := &genxfile.UsesDep{}
	if err := back.Unmarshal(ref); err != nil {
		t.Fatal(err)
	}
	if back.Single.GetName() != "one" || back.Single.GetValue() != 7 {
		t.Fatalf("single mismatch: %+v", back.Single)
	}
	if len(back.Many) != 2 || back.Many[0].GetName() != "a" ||
		back.Many[1].GetName() != "b" || back.Many[1].GetValue() != -1 {
		t.Fatalf("many mismatch: %+v", back.Many)
	}
	if back.Note != "hi" {
		t.Fatalf("note mismatch: %q", back.Note)
	}
	again, err := back.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, ref) {
		t.Fatal("decode/encode of the xfile reference is not byte-exact")
	}
}
