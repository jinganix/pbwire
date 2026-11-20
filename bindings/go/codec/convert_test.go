// Shipped model<->tree conversion: the same native struct the application
// holds moves into and out of a dynamic tree through public API only. pbench
// times exactly these functions, so this test is the contract they rely on.
package codec_test

import (
	"bytes"
	"testing"

	"pbwire/tests/gen/matrix3"
)

func TestConvertModelRoundTrip(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	src := fillScalarsStruct(100)
	ref, err := src.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	tree, err := typ.ImportModel(src, matrix3.Scalars3Meta)
	if err != nil {
		t.Fatal(err)
	}
	wire, err := tree.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(wire, ref) {
		t.Fatalf("import+encode % x, want thin-model % x", wire, ref)
	}
	back, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	var out matrix3.Scalars3
	if err := typ.ExportModel(back, &out, matrix3.Scalars3Meta); err != nil {
		t.Fatal(err)
	}
	if out.Str != "hello" || len(out.Nums) != 100 || out.Inner.GetValue() != 42 {
		t.Fatalf("export lost fields: str=%q nums=%d inner=%+v", out.Str, len(out.Nums), out.Inner)
	}
	if out.OptI32 == nil || *out.OptI32 != -7 {
		t.Fatalf("export lost opt_i32: %+v", out.OptI32)
	}
	again, err := out.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(again, ref) {
		t.Fatalf("export+marshal % x, want % x", again, ref)
	}
}

// Unknown fields survive the tree but have nowhere to go on the struct: the
// export drops them and the re-encode is the known-fields prefix.
func TestConvertModelDropsUnknownOnExport(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	src := fillScalarsStruct(0)
	tree, err := typ.ImportModel(src, matrix3.Scalars3Meta)
	if err != nil {
		t.Fatal(err)
	}
	wire, err := tree.Encode()
	if err != nil {
		t.Fatal(err)
	}
	withUnknown := append(wire, 0xf8, 0x07, 0x01) // field 101, varint 1
	back, err := typ.Decode(withUnknown)
	if err != nil {
		t.Fatal(err)
	}
	var out matrix3.Scalars3
	if err := typ.ExportModel(back, &out, matrix3.Scalars3Meta); err != nil {
		t.Fatal(err)
	}
	if out.Str != "hello" {
		t.Fatalf("export lost known fields: %+v", out.Str)
	}
}
