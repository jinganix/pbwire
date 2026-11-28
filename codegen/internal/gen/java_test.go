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

// The message constructor skips the defensive list copy when the Builder
// slot is empty: absent repeated fields cost no allocation. Primitive
// fields snapshot via toArray, which shares the vacant array by itself.
func TestJavaEmptyListNoCopy(t *testing.T) {
	f := &model.File{
		Path:    "t.proto",
		Package: "t",
		Prefix:  "T",
		Messages: []model.Message{
			{
				Name: "M",
				Full: "t.M",
				Fields: []model.Field{
					{Name: "v", Lower: "v", Pascal: "V", Number: 1, Kind: protoreflect.Int32Kind, Repeated: true},
					{Name: "s", Lower: "s", Pascal: "S", Number: 2, Kind: protoreflect.StringKind, Repeated: true},
				},
			},
		},
	}
	files, err := javaFiles(f, Options{Lang: "java", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := fileNames(files)
	msg := hasFile(t, got, "T_M.java")
	if !strings.Contains(msg, "b.s_.isEmpty()") {
		t.Error("constructor must short-circuit the copy for empty repeated slots")
	}
	if !strings.Contains(msg, "v_ = b.v_.toArray()") {
		t.Error("primitive slots snapshot via toArray")
	}
}

// Repeated numerics ride on primitive storage: the Builder grows an
// IntList (no per-element boxing), the message keeps an exact array, and
// the Meta row carries a primitive getter next to the List one.
func TestJavaRepPrimStorage(t *testing.T) {
	f := &model.File{
		Path:    "t.proto",
		Package: "t",
		Prefix:  "T",
		Messages: []model.Message{
			{
				Name: "M",
				Full: "t.M",
				Fields: []model.Field{
					{Name: "v", Lower: "v", Pascal: "V", Number: 1, Kind: protoreflect.Int32Kind, Repeated: true, Packed: true},
					{Name: "ok", Lower: "ok", Pascal: "Ok", Number: 2, Kind: protoreflect.BoolKind, Repeated: true, Packed: true},
				},
			},
		},
	}
	files, err := javaFiles(f, Options{Lang: "java", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := fileNames(files)
	msg := hasFile(t, got, "T_M.java")
	if !strings.Contains(msg, "PrimLists.Ints v_") {
		t.Error("Builder must grow a primitive list for repeated int")
	}
	if !strings.Contains(msg, "int[] v_;") {
		t.Error("message must keep an exact array for repeated int")
	}
	if !strings.Contains(msg, "boolean[] ok_;") {
		t.Error("message must keep an exact array for repeated bool")
	}
	if !strings.Contains(msg, "getVArray()") {
		t.Error("message must expose the primitive array next to the List view")
	}
	meta := hasFile(t, got, "T_MMeta.java")
	if !strings.Contains(meta, "ofRepIntArr") {
		t.Error("Meta row must carry the primitive int getter")
	}
	if !strings.Contains(meta, "ofRepBoolArr") {
		t.Error("Meta row must carry the primitive bool getter")
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
