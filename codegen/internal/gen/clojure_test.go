package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func clojureTestFile() *model.File {
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

func clojureFileMap(t *testing.T, files []File) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, f := range files {
		out[f.Name] = f.Content
	}
	return out
}

// Default: models plus the schema namespace with eager loading.
func TestClojureFilesDefault(t *testing.T) {
	files, err := clojureFiles(clojureTestFile(), Options{Lang: "clojure", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := clojureFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.clj"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "desc-bytes") || strings.Contains(models, "load-schema-bytes") {
		t.Error("models file must not embed the descriptor")
	}
	if !strings.Contains(models, "(def structtest-inner3-type (pb/message-type meta/schema") {
		t.Error("models resolve types through the meta namespace")
	}
	meta, ok := got["matrix3_meta.clj"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"(def schema (pb/load-schema-bytes desc-bytes))", "message-names", "defn preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema namespace for dynamic-only users.
func TestClojureFilesNoModel(t *testing.T) {
	files, err := clojureFiles(clojureTestFile(), Options{Lang: "clojure", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := clojureFileMap(t, files)
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.clj"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false delays type resolution until init-schema runs.
func TestClojureFilesManualRegister(t *testing.T) {
	files, err := clojureFiles(clojureTestFile(), Options{Lang: "clojure", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := clojureFileMap(t, files)
	models := got["matrix3.clj"]
	if !strings.Contains(models, "(def structtest-inner3-type (delay") {
		t.Error("register=false must delay type resolution")
	}
	if !strings.Contains(models, "@structtest-inner3-type") {
		t.Error("register=false dereferences the delay at use")
	}
	meta := got["matrix3_meta.clj"]
	if !strings.Contains(meta, "call init-schema first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestClojureFilesNoRegistry(t *testing.T) {
	files, err := clojureFiles(clojureTestFile(), Options{Lang: "clojure", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	meta := clojureFileMap(t, files)["matrix3_meta.clj"]
	if strings.Contains(meta, "message-names") || strings.Contains(meta, "defn preload") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(meta, "load-schema-bytes") {
		t.Error("loader stays without the registry")
	}
}
