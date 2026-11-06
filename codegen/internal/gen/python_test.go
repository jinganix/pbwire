package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func pythonTestFile() *model.File {
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

// Default: dataclasses plus the schema module with the lazy load and the list.
func TestPythonFilesDefault(t *testing.T) {
	files, err := pythonFiles(pythonTestFile(), Options{Lang: "python", Model: true, Register: true, Registry: true})
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
	models, ok := got["matrix3_pbwire.py"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "_DESC_B64") || strings.Contains(models, "Schema.load") {
		t.Error("models file must not embed the schema")
	}
	if !strings.Contains(models, "import matrix3_pbwire_meta as _meta") {
		t.Error("models import their schema module")
	}
	meta, ok := got["matrix3_pbwire_meta.py"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"def init_schema", "def use_schema", "def preload", "MESSAGE_NAMES", "_base64.b64decode(_DESC_B64)"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema module for dynamic-only users.
func TestPythonFilesNoModel(t *testing.T) {
	files, err := pythonFiles(pythonTestFile(), Options{Lang: "python", Model: false, Register: true, Registry: false})
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
	if _, ok := got["matrix3_pbwire_meta.py"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; init_schema first, loudly otherwise.
func TestPythonFilesManualRegister(t *testing.T) {
	files, err := pythonFiles(pythonTestFile(), Options{Lang: "python", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_pbwire_meta.py" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "init_schema()") && strings.Contains(meta, "if _SCHEMA is None:\n        init_schema()") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call init_schema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestPythonFilesNoRegistry(t *testing.T) {
	files, err := pythonFiles(pythonTestFile(), Options{Lang: "python", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_pbwire_meta.py" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "MESSAGE_NAMES") || strings.Contains(meta, "def preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "def init_schema") {
		t.Error("loader stays without the registry")
	}
}
