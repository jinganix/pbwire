// Shipped model<->tree conversion for the C core, tested as public API: the
// pbench dync arm times exactly these functions, so this test is the contract
// they rely on.
package dynamic

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	codec "pbwire/runtime"
	"pbwire/tests/gen/matrix3"
)

func testRegistry(t *testing.T) (*Schema, *MessageType) {
	t.Helper()
	_, file, _, _ := runtime.Caller(0)
	raw, err := os.ReadFile(filepath.Join(filepath.Dir(file), "..", "..", "..", "runtime", "testdata", "matrix3.desc"))
	if err != nil {
		t.Fatal(err)
	}
	s, err := Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	typ, err := s.Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(typ.Close)
	return s, typ
}

func fillStruct(n int) *matrix3.Scalars3 {
	i32v, strv := int32(-7), "opt"
	m := &matrix3.Scalars3{
		I32: -2147483648, I64: 9223372036854775807, U32: 4294967295, U64: 18446744073709551615,
		S32: -2147483648, S64: -9223372036854775808, Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: -2147483648, Sfx64: -9223372036854775808, F32: 3.25, F64: -123456.789,
		Flag: true, Str: "hello", Color: matrix3.Color3_COLOR3_GREEN,
		Inner:  &matrix3.Inner3{Name: "kid", Value: 42},
		OptI32: &i32v, OptStr: &strv,
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func TestConvertModelRoundTrip(t *testing.T) {
	_, typ := testRegistry(t)
	src := fillStruct(100)
	ref, err := src.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	tree, err := typ.ImportModel(src, matrix3.Scalars3Meta)
	if err != nil {
		t.Fatal(err)
	}
	defer tree.Close()
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
	defer back.Close()
	var out matrix3.Scalars3
	if err := ExportModel(back, &out, matrix3.Scalars3Meta); err != nil {
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
	_ = codec.KindInt32
}
