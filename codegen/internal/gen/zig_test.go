package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func zigTestFile() *model.File {
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

// Default: models plus the schema file with the name list. Loading stays
// explicit: there is no implicit registry for register to gate.
func TestZigFilesDefault(t *testing.T) {
	files := zigFiles(zigTestFile(), Options{Lang: "zig", Model: true, Register: true, Registry: true})
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.zig"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "const DESC") || strings.Contains(models, "loadSchema") {
		t.Error("models file must not embed the schema")
	}
	meta, ok := got["matrix3_meta.zig"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"const DESC", "pub fn loadSchema", "MESSAGE_NAMES", "pub fn preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestZigFilesNoModel(t *testing.T) {
	files := zigFiles(zigTestFile(), Options{Lang: "zig", Model: false, Register: true, Registry: false})
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.zig"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestZigFilesNoRegistry(t *testing.T) {
	files := zigFiles(zigTestFile(), Options{Lang: "zig", Model: true, Register: true, Registry: false})
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.zig" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "MESSAGE_NAMES") || strings.Contains(meta, "pub fn preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "pub fn loadSchema") {
		t.Error("loader stays without the registry")
	}
}
