package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func juliaTestFile() *model.File {
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
func TestJuliaFilesDefault(t *testing.T) {
	files, err := juliaFiles(juliaTestFile(), Options{Lang: "julia", Model: true, Register: true, Registry: true, DescSetB64: "e30="})
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
	models, ok := got["matrix3_pbwire.jl"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "Pbwire.from_bytes") || strings.Contains(models, "const _DESC") {
		t.Error("models file must not embed the schema")
	}
	if !strings.Contains(models, `include(joinpath(@__DIR__, "matrix3_meta.jl"))`) {
		t.Error("models include their schema file")
	}
	meta, ok := got["matrix3_meta.jl"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"function init_schema", "function use_schema", "MESSAGE_NAMES", "function preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestJuliaFilesNoModel(t *testing.T) {
	files, err := juliaFiles(juliaTestFile(), Options{Lang: "julia", Model: false, Register: true, Registry: false, DescSetB64: "e30="})
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
	if _, ok := got["matrix3_meta.jl"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; init_schema first, loudly otherwise.
func TestJuliaFilesManualRegister(t *testing.T) {
	files, err := juliaFiles(juliaTestFile(), Options{Lang: "julia", Model: true, Register: false, Registry: true, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.jl" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "\n        init_schema()\n") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call init_schema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestJuliaFilesNoRegistry(t *testing.T) {
	files, err := juliaFiles(juliaTestFile(), Options{Lang: "julia", Model: true, Register: true, Registry: false, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.jl" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "MESSAGE_NAMES") || strings.Contains(meta, "function preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "function init_schema") {
		t.Error("loader stays without the registry")
	}
}
