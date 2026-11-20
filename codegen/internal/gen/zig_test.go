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

// Zig has no raw identifiers, so a keyword (or one of the two runtime
// aliases) gets a suffix and non-identifier bytes become underscores.
func TestZigIdent(t *testing.T) {
	cases := map[string]string{
		"struct": "struct_pbwire",
		"std":    "std_pbwire",
		"pbwire": "pbwire_pbwire",
		"a-b":    "a_b",
		"x_file": "x_file",
		"":       "gen",
	}
	for in, want := range cases {
		if got := zigIdent(in); got != want {
			t.Errorf("zigIdent(%q) = %q, want %q", in, got, want)
		}
	}
}

// The in-place arm (plan 10) emits, beside each message, an `extern struct`
// mirror of the C struct codec's image plus a comptime `fields` row table.
// The mirror's offsets are asserted at comptime, so an emitter/C ABI mismatch
// is a compile error, not bad bytes.
func TestZigLayout(t *testing.T) {
	f := &model.File{
		Path:    "matrix3.proto",
		Package: "structtest",
		Prefix:  "Structtest",
		Messages: []model.Message{
			{
				Name: "Scalars3",
				Full: "structtest.Scalars3",
				Fields: []model.Field{
					{Name: "i32", Lower: "i32", Pascal: "I32", Number: 1, Kind: protoreflect.Int32Kind},
					{Name: "str", Lower: "str", Pascal: "Str", Number: 14, Kind: protoreflect.StringKind},
					{Name: "nums", Lower: "nums", Pascal: "Nums", Number: 17, Kind: protoreflect.Int32Kind, Repeated: true},
					{Name: "opt_i32", Lower: "optI32", Pascal: "OptI32", Number: 20, Kind: protoreflect.Int32Kind, NeedsHas: true},
				},
			},
		},
	}
	files := zigFiles(f, Options{Lang: "zig", Model: true, Register: true, Registry: false})
	var models string
	for _, file := range files {
		if file.Name == "matrix3.zig" {
			models = file.Content
		}
	}
	for _, want := range []string{
		"pub const Scalars3Mirror = extern struct {",
		"pub const fields = [_]FieldPlan{",
		".kind = .int32",
		".repeated = true",
		"@offsetOf(Scalars3Mirror, \"opt_i32\")",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("zig layout misses %q\n%s", want, models)
		}
	}
}
