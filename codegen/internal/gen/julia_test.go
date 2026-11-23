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

// The layout gate (plan 07 Stage 1): every message that the flat layout can
// express gets a mutable `isbits`-field image plus the offset/row table the
// C-side reader consumes. A missing struct or a row without its `fieldoffset`
// means the shim arm has no native object to time, so the test fails before
// any number is published.
func TestJuliaLayout(t *testing.T) {
	f := &model.File{
		Path:    "matrix3.proto",
		Package: "structtest",
		Prefix:  "Structtest",
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
	files, err := juliaFiles(f, Options{Lang: "julia", Model: true, Register: true, Registry: true, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	var models string
	for _, file := range files {
		if file.Name == "matrix3_pbwire.jl" {
			models = file.Content
		}
	}
	if models == "" {
		t.Fatalf("missing the models file, got %v", files)
	}
	for _, want := range []string{
		"mutable struct Scalars3Layout",
		"i32::Int32",
		"str::Ptr{UInt8}",
		"inner::Ptr{Inner3Layout}",
		"nums::Ptr{Int32}",
		"numsCount::Csize_t",
		"const scalars3LayoutRows = Pbwire.LayoutRow[",
		"fieldoffset(Scalars3Layout, :i32)",
		"fieldoffset(Scalars3Layout, :str)",
		"fieldoffset(Scalars3Layout, :inner)",
		"fieldoffset(Scalars3Layout, :has_opt_i32)",
		"fieldoffset(Scalars3Layout, :nums)",
		"fieldoffset(Scalars3Layout, :numsCount)",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("models file misses %q", want)
		}
	}
}
