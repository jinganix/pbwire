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
