package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func goTestFile() *model.File {
	return &model.File{
		Path:    "t.proto",
		Package: "t",
		Prefix:  "T",
		Messages: []model.Message{
			{
				Name: "M",
				Full: "t.M",
				Fields: []model.Field{
					{Name: "v", Lower: "v", Pascal: "V", Number: 1, Kind: protoreflect.Int32Kind},
				},
			},
		},
	}
}

func goFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the schema file with implicit load and the name list.
func TestGoFilesDefault(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want model+meta files, got %v", keys(got))
	}
	models, ok := got["t_pbwire.go"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "_DESC_B64") {
		t.Error("models file must not embed the descriptor")
	}
	meta, ok := got["t_pbwire_meta.go"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"MessageNames", "func Preload()", "func Init()", "func UseSchema(", "schemaOnce.Do"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false: schema file only, for dynamic-only users.
func TestGoFilesNoModel(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["t_pbwire_meta.go"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false: no implicit load; Init or UseSchema first, loudly otherwise.
func TestGoFilesManualRegister(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	meta := goFileMap(t, files)["t_pbwire_meta.go"]
	if strings.Contains(meta, "schemaOnce.Do") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(meta, "call Init or UseSchema first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and Preload; the loader stays.
func TestGoFilesNoRegistry(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	meta := goFileMap(t, files)["t_pbwire_meta.go"]
	if strings.Contains(meta, "MessageNames") || strings.Contains(meta, "func Preload()") {
		t.Error("registry=false must drop the name list")
	}
	if !strings.Contains(meta, "func getType(") {
		t.Error("loader stays without the registry")
	}
}
