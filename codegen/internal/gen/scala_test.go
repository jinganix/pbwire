package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func scalaTestFile() *model.File {
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
					{Name: "value", Lower: "value", Pascal: "Value", Number: 2, Kind: protoreflect.Int32Kind},
				},
			},
		},
	}
}

func scalaFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the schema object with eager loading and the list.
func TestScalaFilesDefault(t *testing.T) {
	files, err := scalaFiles(scalaTestFile(), Options{Lang: "scala", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := scalaFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.scala"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "Schema.fromBytes") {
		t.Error("models file must not embed the descriptor")
	}
	meta, ok := got["matrix3_meta.scala"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"initSchema()", "messageNames", "def preload", "def schema"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema object for dynamic-only users.
func TestScalaFilesNoModel(t *testing.T) {
	files, err := scalaFiles(scalaTestFile(), Options{Lang: "scala", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := scalaFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.scala"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the eager load; initSchema first, loudly otherwise.
func TestScalaFilesManualRegister(t *testing.T) {
	files, err := scalaFiles(scalaTestFile(), Options{Lang: "scala", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	meta := scalaFileMap(t, files)["matrix3_meta.scala"]
	if strings.Contains(meta, "\n  initSchema()\n") {
		t.Error("register=false must drop the eager load")
	}
	if !strings.Contains(meta, "call initSchema first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the message list and preload; the loader stays.
func TestScalaFilesNoRegistry(t *testing.T) {
	files, err := scalaFiles(scalaTestFile(), Options{Lang: "scala", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	meta := scalaFileMap(t, files)["matrix3_meta.scala"]
	if strings.Contains(meta, "messageNames") || strings.Contains(meta, "def preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "def schema") {
		t.Error("loader stays without the registry")
	}
}
