package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func swiftTestFile() *model.File {
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

// Default: models plus the schema file with the lazy schema and the list.
func TestSwiftFilesDefault(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: true, Registry: true})
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
	models, ok := got["matrix3.swift"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "descData") || strings.Contains(models, "Data(base64Encoded") {
		t.Error("models file must not embed the descriptor")
	}
	if !strings.Contains(models, "matrix3_schema.message") {
		t.Error("models resolve through the stem-prefixed schema")
	}
	meta, ok := got["matrix3_meta.swift"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"_matrix3_embedded", "func matrix3_initSchema", "func matrix3_useSchema", "var matrix3_schema", "matrix3_messageNames", "func matrix3_preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestSwiftFilesNoModel(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: false, Register: true, Registry: false})
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
	if _, ok := got["matrix3_meta.swift"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; initSchema or useSchema first.
func TestSwiftFilesManualRegister(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.swift" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "_matrix3_embedded") {
		t.Error("register=false must drop the embedded lazy schema")
	}
	if !strings.Contains(meta, "call matrix3_initSchema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestSwiftFilesNoRegistry(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.swift" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "messageNames") || strings.Contains(meta, "preload") {
		t.Error("registry=false must drop the list")
	}
}
