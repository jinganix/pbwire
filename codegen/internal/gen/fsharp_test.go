package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func fsharpTestFile() *model.File {
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

func fsharpFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the schema module with eager load and the list.
func TestFSharpFilesDefault(t *testing.T) {
	files, err := fsharpFiles(fsharpTestFile(), Options{Lang: "fsharp", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := fsharpFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.fs"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "FromBase64String") || strings.Contains(models, "let desc") {
		t.Error("models file must not embed the descriptor")
	}
	meta, ok := got["matrix3_meta.fs"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"do initSchema desc", "let initSchema", "let useSchema", "let getSchema", "messageNames", "let preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema module for dynamic-only users.
func TestFSharpFilesNoModel(t *testing.T) {
	files, err := fsharpFiles(fsharpTestFile(), Options{Lang: "fsharp", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := fsharpFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.fs"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the eager load; initSchema or useSchema first.
func TestFSharpFilesManualRegister(t *testing.T) {
	files, err := fsharpFiles(fsharpTestFile(), Options{Lang: "fsharp", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	meta := fsharpFileMap(t, files)["matrix3_meta.fs"]
	if strings.Contains(meta, "do initSchema desc") {
		t.Error("register=false must drop the eager load")
	}
	if !strings.Contains(meta, "call initSchema or useSchema first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestFSharpFilesNoRegistry(t *testing.T) {
	files, err := fsharpFiles(fsharpTestFile(), Options{Lang: "fsharp", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	meta := fsharpFileMap(t, files)["matrix3_meta.fs"]
	if strings.Contains(meta, "messageNames") || strings.Contains(meta, "let preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "let initSchema") {
		t.Error("loader stays without the registry")
	}
}
