package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func phpTestFile() *model.File {
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

// Default: models plus the holder with the lazy load and the name list.
func TestPhpFilesDefault(t *testing.T) {
	files, err := phpFiles(phpTestFile(), Options{Lang: "php", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 2 {
		t.Fatalf("want models+holder files, got %v", keys(got))
	}
	models, ok := got["matrix3.php"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "DESC_B64") || strings.Contains(models, "fromBytes") {
		t.Error("models file must not embed the schema")
	}
	holder, ok := got["PbwireFile_Matrix3.php"]
	if !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
	for _, want := range []string{"private const DESC_B64", "public static function init()", "public static function use_schema", "MESSAGE_NAMES", "public static function preload()"} {
		if !strings.Contains(holder, want) {
			t.Errorf("holder misses %q", want)
		}
	}
}

// model=false leaves the holder for dynamic-only users.
func TestPhpFilesNoModel(t *testing.T) {
	files, err := phpFiles(phpTestFile(), Options{Lang: "php", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 1 {
		t.Fatalf("want holder file only, got %v", keys(got))
	}
	if _, ok := got["PbwireFile_Matrix3.php"]; !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
}

// register=false drops the implicit load; init or use_schema first.
func TestPhpFilesManualRegister(t *testing.T) {
	files, err := phpFiles(phpTestFile(), Options{Lang: "php", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var holder string
	for _, f := range files {
		if f.Name == "PbwireFile_Matrix3.php" {
			holder = f.Content
		}
	}
	if strings.Contains(holder, "self::init();") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(holder, "call init() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestPhpFilesNoRegistry(t *testing.T) {
	files, err := phpFiles(phpTestFile(), Options{Lang: "php", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var holder string
	for _, f := range files {
		if f.Name == "PbwireFile_Matrix3.php" {
			holder = f.Content
		}
	}
	if strings.Contains(holder, "MESSAGE_NAMES") || strings.Contains(holder, "function preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(holder, "public static function init()") {
		t.Error("loader stays without the registry")
	}
}
