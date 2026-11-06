package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func elixirTestFile() *model.File {
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
		Enums: []model.Enum{
			{Name: "Color3", Full: "structtest.Color3", Values: []model.EnumValue{{Name: "RED", Number: 1}}},
		},
	}
}

// Default: modules plus the Registry list; no descriptor sidecar exists.
func TestElixirFilesDefault(t *testing.T) {
	files, err := elixirFiles(elixirTestFile(), Options{Lang: "elixir", Model: true, Register: true, Registry: true, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	names := []string{}
	for _, f := range files {
		names = append(names, f.Name)
	}
	if len(files) != 1 || files[0].Name != "matrix3.ex" {
		t.Fatalf("want one file, got %v", names)
	}
	got := files[0].Content
	if !strings.Contains(got, "defmodule Structtest.Inner3 do") {
		t.Error("modules are the schema")
	}
	if !strings.Contains(got, "defmodule Structtest.Matrix3.Registry do") {
		t.Error("registry is per-file when the stem differs from the package")
	}
	if !strings.Contains(got, "def message_modules do") {
		t.Error("registry holds the module list")
	}
}

// registry=false drops the list; modules stay.
func TestElixirFilesNoRegistry(t *testing.T) {
	files, err := elixirFiles(elixirTestFile(), Options{Lang: "elixir", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := files[0].Content
	if strings.Contains(got, "Registry") || strings.Contains(got, "message_modules") {
		t.Error("registry=false must drop the list")
	}
	if !strings.Contains(got, "defmodule Structtest.Inner3 do") {
		t.Error("modules stay without the registry")
	}
}

// model=false leaves the list without modules.
func TestElixirFilesNoModel(t *testing.T) {
	files, err := elixirFiles(elixirTestFile(), Options{Lang: "elixir", Model: false, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := files[0].Content
	if strings.Contains(got, "defmodule Structtest.Inner3 do") {
		t.Error("model=false must drop the modules")
	}
	if !strings.Contains(got, "def message_modules do") {
		t.Error("list stays without the models")
	}
}
