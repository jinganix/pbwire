package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func rTestFile() *model.File {
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

// Default: models plus the schema file with the lazy load and the list.
func TestRFilesDefault(t *testing.T) {
	files, err := rFiles(rTestFile(), Options{Lang: "r", Model: true, Register: true, Registry: true})
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
	models, ok := got["matrix3_pbwire.R"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "pbwire_load_raw") || strings.Contains(models, ".pbwire_desc") {
		t.Error("models file must not embed the schema")
	}
	meta, ok := got["matrix3_meta.R"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{".pbwire_init_schema_matrix3", ".pbwire_use_schema_matrix3", ".pbwire_message_names_matrix3", ".pbwire_preload_matrix3"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestRFilesNoModel(t *testing.T) {
	files, err := rFiles(rTestFile(), Options{Lang: "r", Model: false, Register: true, Registry: false})
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
	if _, ok := got["matrix3_meta.R"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; init_schema first, loudly otherwise.
func TestRFilesManualRegister(t *testing.T) {
	files, err := rFiles(rTestFile(), Options{Lang: "r", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.R" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "\n    .pbwire_init_schema_matrix3()\n") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call .pbwire_init_schema_matrix3() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestRFilesNoRegistry(t *testing.T) {
	files, err := rFiles(rTestFile(), Options{Lang: "r", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.R" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "message_names") || strings.Contains(meta, "preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "init_schema") {
		t.Error("loader stays without the registry")
	}
}
