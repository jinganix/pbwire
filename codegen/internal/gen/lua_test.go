package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func luaTestFile() *model.File {
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

// Default: models plus the schema module with the lazy load and the list.
func TestLuaFilesDefault(t *testing.T) {
	files, err := luaFiles(luaTestFile(), Options{Lang: "lua", DescSetB64: "e30=", Model: true, Register: true, Registry: true})
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
	models, ok := got["matrix3.lua"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "pbwire.load(") || strings.Contains(models, "local DESC") {
		t.Error("models file must not embed the schema")
	}
	if !strings.Contains(models, "require(\"matrix3_meta\")") {
		t.Error("models import their schema module")
	}
	if !strings.Contains(models, "meta.schema()") {
		t.Error("models resolve through the meta module")
	}
	meta, ok := got["matrix3_meta.lua"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"init_schema", "use_schema", "MESSAGE_NAMES", "preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema module for dynamic-only users.
func TestLuaFilesNoModel(t *testing.T) {
	files, err := luaFiles(luaTestFile(), Options{Lang: "lua", DescSetB64: "e30=", Model: false, Register: true, Registry: false})
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
	if _, ok := got["matrix3_meta.lua"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; init_schema first, loudly otherwise.
func TestLuaFilesManualRegister(t *testing.T) {
	files, err := luaFiles(luaTestFile(), Options{Lang: "lua", DescSetB64: "e30=", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.lua" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "if _schema == nil then _schema = pbwire.load(DESC) end") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call init_schema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestLuaFilesNoRegistry(t *testing.T) {
	files, err := luaFiles(luaTestFile(), Options{Lang: "lua", DescSetB64: "e30=", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.lua" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "MESSAGE_NAMES") || strings.Contains(meta, "preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "init_schema") {
		t.Error("loader stays without the registry")
	}
}

// Sibling modules are required under a prefixed alias: a dep file named after
// a Lua keyword (or after one of the file's own locals, e.g. meta) would
// otherwise shadow it or fail to parse.
func TestLuaDepAlias(t *testing.T) {
	cases := map[string]string{
		"xdep.proto":      "_imp_xdep",
		"a-dir/a-b.proto": "_imp_a_b",
		"end.proto":       "_imp_end",
		"meta.proto":      "_imp_meta",
	}
	for in, want := range cases {
		if got := luaDepAlias(in); got != want {
			t.Errorf("luaDepAlias(%q) = %q, want %q", in, got, want)
		}
	}
}
