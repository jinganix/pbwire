package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func dartTestFile() *model.File {
	return &model.File{
		Path:    "matrix3.proto",
		Package: "structtest",
		Prefix:  "Structtest",
		Messages: []model.Message{
			{
				Name: "Inner3",
				Full: "structtest.Inner3",
				Fields: []model.Field{
					{Name: "name", Lower: "name", Pascal: "Name", Number: 1, Kind: protoreflect.StringKind},
				},
			},
		},
	}
}

// Default: models plus the schema library with the lazy load and the list.
func TestDartFilesDefault(t *testing.T) {
	files, err := dartFiles(dartTestFile(), Options{Lang: "dart", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.dart"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "_descB64") || strings.Contains(models, "base64Decode") {
		t.Error("models file must not embed the descriptor")
	}
	if !strings.Contains(models, "matrix3_schema.message") {
		t.Error("models resolve through the stem-prefixed schema")
	}
	if !strings.Contains(models, "import 'matrix3_meta.dart';") {
		t.Error("models import their schema library")
	}
	meta, ok := got["matrix3_meta.dart"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"_descB64", "matrix3_initSchema", "matrix3_useSchema", "matrix3_schema", "matrix3_messageNames", "matrix3_preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema library for dynamic-only users.
func TestDartFilesNoModel(t *testing.T) {
	files, err := dartFiles(dartTestFile(), Options{Lang: "dart", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.dart"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; initSchema or useSchema first.
func TestDartFilesManualRegister(t *testing.T) {
	files, err := dartFiles(dartTestFile(), Options{Lang: "dart", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.dart" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "_schema ??=") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call matrix3_initSchema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestDartFilesNoRegistry(t *testing.T) {
	files, err := dartFiles(dartTestFile(), Options{Lang: "dart", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.dart" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "messageNames") || strings.Contains(meta, "preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "matrix3_initSchema") {
		t.Error("loader stays without the registry")
	}
}

// Sibling imports are prefixed: the generated file shares its namespace with
// the pbwire runtime, so a message named Value in a dep file would otherwise
// make every reference ambiguous.
func TestDartImpPrefix(t *testing.T) {
	cases := map[string]string{
		"xdep.proto":       "_imp_xdep",
		"a-dir/a-b.proto":  "_imp_a_b",
		"google/x_1.proto": "_imp_x_1",
	}
	for in, want := range cases {
		if got := dartImpPrefix(in); got != want {
			t.Errorf("dartImpPrefix(%q) = %q, want %q", in, got, want)
		}
	}
}

// The layout gate (plan 06 Stage 1): every message gets a flat `dart:ffi`
// Struct plus the offset/row table the C-side reader consumes. A missing
// class, a missing row or a row without its offset means the shim arm has no
// native object to time, so the test fails before any number is published.
func TestDartLayout(t *testing.T) {
	f := &model.File{
		Path:    "matrix3.proto",
		Package: "structtest",
		Messages: []model.Message{
			{
				Name: "Inner3",
				Full: "structtest.Inner3",
				Fields: []model.Field{
					{Name: "name", Lower: "name", Pascal: "Name", Number: 1, Kind: protoreflect.StringKind},
					{Name: "value", Lower: "value", Pascal: "Value", Number: 2, Kind: protoreflect.Int32Kind},
				},
			},
			{
				Name: "Scalars3",
				Full: "structtest.Scalars3",
				Fields: []model.Field{
					{Name: "i32", Lower: "i32", Pascal: "I32", Number: 1, Kind: protoreflect.Int32Kind},
					{Name: "str", Lower: "str", Pascal: "Str", Number: 14, Kind: protoreflect.StringKind},
					{Name: "inner", Lower: "inner", Pascal: "Inner", Number: 16, Kind: protoreflect.MessageKind, Message: "Inner3", Full: "structtest.Inner3"},
					{Name: "opt_i32", Lower: "optI32", Pascal: "OptI32", Number: 20, Kind: protoreflect.Int32Kind, NeedsHas: true},
					{Name: "nums", Lower: "nums", Pascal: "Nums", Number: 17, Kind: protoreflect.Int32Kind, Repeated: true, Packed: true},
				},
			},
		},
	}
	files, err := dartFiles(f, Options{Lang: "dart", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var models string
	for _, file := range files {
		if file.Name == "matrix3.dart" {
			models = file.Content
		}
	}
	if models == "" {
		t.Fatalf("missing the models file, got %v", files)
	}
	for _, want := range []string{
		"import 'dart:ffi';",
		"final class Scalars3Layout extends Struct",
		"@Int32() external int i32;",
		"external Pointer<Utf8> str;",
		"external Pointer<Inner3Layout> inner;",
		"@Int32() external int optI32;",
		"@Int32() external int _hasOptI32;",
		"external Pointer<Int32> nums;",
		"@IntPtr() external int numsCount;",
		"static const List<int> fieldOffsets = <int>[0, 8, 16, 24, 32];",
		"static int get layoutSize => sizeOf<Scalars3Layout>();",
		"const List<PbwField> scalars3LayoutRows = <PbwField>[",
		"PbwField(number: 1, kind: PbwKind.int32, offset: 0, type: 5, wire: 0),",
		"PbwField(number: 16, kind: PbwKind.message, offset: 16, child: 'structtest.Inner3', type: 11, wire: 2),",
		"PbwField(number: 20, kind: PbwKind.int32, offset: 24, has: 28, type: 5, wire: 0),",
		"PbwField(number: 17, kind: PbwKind.int32, offset: 32, repeated: true, count: 40, type: 5, wire: 0, packed: true),",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("models file misses %q", want)
		}
	}
}
