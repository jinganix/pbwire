package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func objcTestFile() *model.File {
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

// Default: models plus the schema holder with the lazy load and the list.
func TestObjcFilesDefault(t *testing.T) {
	files, err := objcFiles(objcTestFile(), Options{Lang: "objc", Model: true, Register: true, Registry: true, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 4 {
		t.Fatalf("want models pair + holder pair, got %v", keys(got))
	}
	models, ok := got["matrix3.m"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "kPBWGenMatrix3FileSchemaDescB64") || strings.Contains(models, "dispatch_once") {
		t.Error("models file must not embed the schema")
	}
	holder, ok := got["matrix3_schema.m"]
	if !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
	for _, want := range []string{"PBWGenMatrix3FileSchemaInit", "PBWGenMatrix3FileSchemaUse", "PBWGenMatrix3FileSchemaMessageNames", "PBWGenMatrix3FileSchemaPreload"} {
		if !strings.Contains(holder, want) {
			t.Errorf("holder misses %q", want)
		}
	}
}

// model=false leaves the schema holder for dynamic-only users.
func TestObjcFilesNoModel(t *testing.T) {
	files, err := objcFiles(objcTestFile(), Options{Lang: "objc", Model: false, Register: true, Registry: false, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 2 {
		t.Fatalf("want holder pair only, got %v", keys(got))
	}
	if _, ok := got["matrix3_schema.m"]; !ok {
		t.Fatalf("missing holder file, got %v", keys(got))
	}
}

// register=false drops the implicit load; Init/Use first, loudly otherwise.
func TestObjcFilesManualRegister(t *testing.T) {
	files, err := objcFiles(objcTestFile(), Options{Lang: "objc", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var holder string
	for _, f := range files {
		if f.Name == "matrix3_schema.m" {
			holder = f.Content
		}
	}
	if strings.Contains(holder, "dispatch_once") {
		t.Error("register=false must drop the implicit load")
	}
	if !strings.Contains(holder, "NSException") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestObjcFilesNoRegistry(t *testing.T) {
	files, err := objcFiles(objcTestFile(), Options{Lang: "objc", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var holder string
	for _, f := range files {
		if f.Name == "matrix3_schema.m" {
			holder = f.Content
		}
	}
	if strings.Contains(holder, "MessageNames") || strings.Contains(holder, "Preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(holder, "PBWGenMatrix3FileSchemaInit") {
		t.Error("loader stays without the registry")
	}
}
