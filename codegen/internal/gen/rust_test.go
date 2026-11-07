package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func rustTestFile() *model.File {
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
			{
				Name: "Scalars3",
				Full: "structtest.Scalars3",
				Fields: []model.Field{
					{Name: "i32", Lower: "i32", Pascal: "I32", Number: 1, Kind: protoreflect.Int32Kind},
				},
			},
		},
	}
}

func rustFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the schema file with the lazy per-thread schema.
func TestRustFilesDefault(t *testing.T) {
	files := rustFiles(rustTestFile(), Options{Lang: "rust", Model: true, Register: true, Registry: true})
	got := rustFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3_pbwire.rs"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "static DESC") || strings.Contains(models, "thread_local") {
		t.Error("models file must not embed the schema")
	}
	meta, ok := got["matrix3_pbwire_meta.rs"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"static DESC", "static SCHEMA: Schema", "MESSAGE_NAMES", "pub fn preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestRustFilesNoModel(t *testing.T) {
	files := rustFiles(rustTestFile(), Options{Lang: "rust", Model: false, Register: true, Registry: false})
	got := rustFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_pbwire_meta.rs"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit schema; init() first, loudly otherwise.
func TestRustFilesManualRegister(t *testing.T) {
	files := rustFiles(rustTestFile(), Options{Lang: "rust", Model: true, Register: false, Registry: true})
	meta := rustFileMap(t, files)["matrix3_pbwire_meta.rs"]
	if strings.Contains(meta, "static SCHEMA: Schema =") {
		t.Error("register=false must drop the implicit schema")
	}
	if !strings.Contains(meta, "pub fn init()") || !strings.Contains(meta, "call init() first") {
		t.Error("register=false needs explicit init with a loud error")
	}
	models := rustFileMap(t, files)["matrix3_pbwire.rs"]
	if !strings.Contains(models, "meta::schema(|s|") {
		t.Error("register=false routes entry points through the checked loader")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestRustFilesNoRegistry(t *testing.T) {
	files := rustFiles(rustTestFile(), Options{Lang: "rust", Model: true, Register: true, Registry: false})
	meta := rustFileMap(t, files)["matrix3_pbwire_meta.rs"]
	if strings.Contains(meta, "MESSAGE_NAMES") || strings.Contains(meta, "pub fn preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "static SCHEMA: Schema =") {
		t.Error("loader stays without the registry")
	}
}

// A message or dep file named after a Rust keyword yields a module segment
// that needs a raw identifier; crate/self/super cannot be raw and get a
// suffix instead.
func TestRustModSegment(t *testing.T) {
	cases := map[string]string{
		"struct": "r#struct",
		"match":  "r#match",
		"type":   "r#type",
		"crate":  "crate_pbwire",
		"self":   "self_pbwire",
		"super":  "super_pbwire",
		"order":  "order",
	}
	for in, want := range cases {
		if got := rustModSegment(in); got != want {
			t.Errorf("rustModSegment(%q) = %q, want %q", in, got, want)
		}
	}
}
