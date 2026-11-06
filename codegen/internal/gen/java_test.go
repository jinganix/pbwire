package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// Minimal fixture: two messages, no imports, no nesting.
func testFile() *model.File {
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
			{
				Name: "N",
				Full: "t.N",
				Fields: []model.Field{
					{Name: "s", Lower: "s", Pascal: "S", Number: 1, Kind: protoreflect.StringKind},
				},
			},
		},
	}
}

func fileNames(files []File) map[string]string {
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

func hasFile(t *testing.T, files map[string]string, name string) string {
	t.Helper()
	c, ok := files[name]
	if !ok {
		t.Fatalf("missing file %s (got %v)", name, keys(files))
	}
	return c
}

func keys(m map[string]string) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	return out
}

// Default: models that self-register, plus the whole-file list.
func TestJavaFilesDefault(t *testing.T) {
	files, err := javaFiles(testFile(), Options{Lang: "java", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := fileNames(files)
	msg := hasFile(t, got, "T_M.java")
	if !strings.Contains(msg, "static {") {
		t.Error("default message self-registers")
	}
	meta := hasFile(t, got, "T_MMeta.java")
	if !strings.Contains(meta, "META = forBuilder") {
		t.Error("default meta binds the generated Builder")
	}
	reg := hasFile(t, got, "T_Registry.java")
	if !strings.Contains(reg, "T_M.class") || !strings.Contains(reg, "T_N.class") {
		t.Error("registry lists every message")
	}
}

// register=false: the message ships without the static block; the register
// helper stays for explicit (deferred or custom) registration.
func TestJavaFilesManualRegister(t *testing.T) {
	files, err := javaFiles(testFile(), Options{Lang: "java", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := fileNames(files)
	msg := hasFile(t, got, "T_M.java")
	if strings.Contains(msg, "static {") {
		t.Error("register=false must drop the static block")
	}
	if !strings.Contains(hasFile(t, got, "T_MMeta.java"), "forBuilder(Class<B> builderClass)") {
		t.Error("forBuilder stays for manual registration")
	}
}

// model=false: interfaces plus tables only, for hand-written models.
func TestJavaFilesNoModel(t *testing.T) {
	files, err := javaFiles(testFile(), Options{Lang: "java", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := fileNames(files)
	if len(got) != 4 {
		t.Fatalf("model=false emits OrBuilder+Meta only (got %v)", keys(got))
	}
	meta := hasFile(t, got, "T_MMeta.java")
	if strings.Contains(meta, "META = forBuilder") {
		t.Error("model=false has no generated Builder to bind")
	}
}

// registry without model has no classes to list.
func TestJavaFilesRegistryNeedsModel(t *testing.T) {
	if _, err := javaFiles(testFile(), Options{Lang: "java", Model: false, Register: true, Registry: true}); err == nil {
		t.Error("registry=true needs model=true")
	}
}

// The file stem keeps registries distinct when a package spans files;
// a file named after its package keeps the short form.
func TestRegistryName(t *testing.T) {
	f := testFile()
	if got := registryName(f); got != "T_Registry" {
		t.Errorf("short form: got %s", got)
	}
	f.Path = "sub/two.proto"
	if got := registryName(f); got != "T_Two_Registry" {
		t.Errorf("stem form: got %s", got)
	}
}
