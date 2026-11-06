package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func csharpTestFile() *model.File {
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
					{Name: "inner", Lower: "inner", Pascal: "Inner", Number: 2, Kind: protoreflect.MessageKind, Message: "Inner3"},
				},
			},
		},
	}
}

func csharpFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the holder with eager load and the name list.
func TestCSharpFilesDefault(t *testing.T) {
	files, err := csharpFiles(csharpTestFile(), Options{Lang: "csharp", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := csharpFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want models+holder files, got %v", keys(got))
	}
	models, ok := got["matrix3.cs"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "FromBase64String") || strings.Contains(models, "internal static") {
		t.Error("models file must not embed the schema")
	}
	holder, ok := got["PbwireFile_Matrix3.cs"]
	if !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
	for _, want := range []string{"static PbwireFile_Matrix3() { Init(); }", "public static void Init()", "public static void UseSchema(Schema? s)", "MessageNames", "public static void Preload()"} {
		if !strings.Contains(holder, want) {
			t.Errorf("holder misses %q", want)
		}
	}
}

// model=false leaves the holder for dynamic-only users.
func TestCSharpFilesNoModel(t *testing.T) {
	files, err := csharpFiles(csharpTestFile(), Options{Lang: "csharp", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := csharpFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want holder file only, got %v", keys(got))
	}
	if _, ok := got["PbwireFile_Matrix3.cs"]; !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
}

// register=false drops the eager static constructor; Init or UseSchema first.
func TestCSharpFilesManualRegister(t *testing.T) {
	files, err := csharpFiles(csharpTestFile(), Options{Lang: "csharp", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	holder := csharpFileMap(t, files)["PbwireFile_Matrix3.cs"]
	if strings.Contains(holder, "static PbwireFile_Matrix3()") {
		t.Error("register=false must drop the eager static constructor")
	}
	if !strings.Contains(holder, "call Init or UseSchema first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestCSharpFilesNoRegistry(t *testing.T) {
	files, err := csharpFiles(csharpTestFile(), Options{Lang: "csharp", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	holder := csharpFileMap(t, files)["PbwireFile_Matrix3.cs"]
	if strings.Contains(holder, "MessageNames") || strings.Contains(holder, "Preload()") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(holder, "public static void Init()") {
		t.Error("loader stays without the registry")
	}
}
