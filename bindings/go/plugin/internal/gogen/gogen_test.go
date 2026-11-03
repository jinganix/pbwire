package gogen_test

import (
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"pbwire-go-plugin/internal/core"
	"pbwire-go-plugin/internal/gogen"
	"pbwire-go-plugin/internal/testutil"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/types/descriptorpb"
)

// Each golden is a pinned generated file the Go example/tests compile. The
// generator output must match it byte for byte; `make -C bindings/go/plugin
// regen` rewrites them from the same protoc requests the C plugin replays.
func TestGoldens(t *testing.T) {
	cases := []struct {
		dump string // testdata dir holding the replayed test.dump
		name string // expected output file name
		want string // pinned file, relative to the repo root
	}{
		{"addressbook", "addressbook.pbwire.go", filepath.Join("bindings", "go", "examples", "addressbook", "tutorial", "addressbook.pbwire.go")},
		{"matrix3", "matrix3.pbwire.go", filepath.Join("bindings", "go", "tests", "gen", "structtest", "matrix3.pbwire.go")},
		{"matrix2", "matrix2.pbwire.go", filepath.Join("bindings", "go", "tests", "gen", "structtest", "matrix2.pbwire.go")},
		{"edges", "edges.pbwire.go", filepath.Join("bindings", "go", "tests", "gen", "edgetest", "edges.pbwire.go")},
	}
	for _, tc := range cases {
		t.Run(tc.dump, func(t *testing.T) {
			req, err := testutil.LoadRequest(filepath.Join(testutil.RepoRoot(),
				"bindings", "go", "plugin", "testdata", tc.dump, "test.dump"))
			if err != nil {
				t.Fatal(err)
			}
			ctx, err := core.NewRequestContext(req)
			if err != nil {
				t.Fatal(err)
			}
			if len(ctx.TargetDescriptors) != 1 {
				t.Fatalf("targets = %d, want 1", len(ctx.TargetDescriptors))
			}
			name, got, err := gogen.Generate(ctx.TargetDescriptors[0], gogen.Options{})
			if err != nil {
				t.Fatal(err)
			}
			if name != tc.name {
				t.Fatalf("name = %q, want %q", name, tc.name)
			}
			want, err := testutil.ReadFile(tc.want)
			if err != nil {
				t.Fatal(err)
			}
			if got != want {
				t.Fatalf("generated output differs from %s; run `make -C bindings/go/plugin regen` and inspect the diff", tc.want)
			}
		})
	}
}

func TestRejectsUnsupportedFields(t *testing.T) {
	cases := []struct {
		name string
		fd   *descriptorpb.FileDescriptorProto
		want string
	}{
		{
			name: "map",
			fd: file("map.proto", &descriptorpb.DescriptorProto{
				Name: proto.String("M"),
				NestedType: []*descriptorpb.DescriptorProto{{
					Name:    proto.String("LabelsEntry"),
					Options: &descriptorpb.MessageOptions{MapEntry: proto.Bool(true)},
					Field: []*descriptorpb.FieldDescriptorProto{
						field("key", 1, descriptorpb.FieldDescriptorProto_TYPE_STRING),
						field("value", 2, descriptorpb.FieldDescriptorProto_TYPE_INT32),
					},
				}},
				Field: []*descriptorpb.FieldDescriptorProto{{
					Name:     proto.String("labels"),
					Number:   proto.Int32(1),
					Label:    descriptorpb.FieldDescriptorProto_LABEL_REPEATED.Enum(),
					Type:     descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(),
					TypeName: proto.String(".t.M.LabelsEntry"),
				}},
			}),
			want: "map fields are not supported",
		},
		{
			name: "oneof",
			fd: file("oneof.proto", &descriptorpb.DescriptorProto{
				Name: proto.String("M"),
				OneofDecl: []*descriptorpb.OneofDescriptorProto{{
					Name: proto.String("choice"),
				}},
				Field: []*descriptorpb.FieldDescriptorProto{{
					Name:       proto.String("a"),
					Number:     proto.Int32(1),
					Label:      descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
					Type:       descriptorpb.FieldDescriptorProto_TYPE_INT32.Enum(),
					OneofIndex: proto.Int32(0),
				}},
			}),
			want: "oneof fields are not supported",
		},
		{
			name: "group",
			fd: func() *descriptorpb.FileDescriptorProto {
				fd := file("group.proto", &descriptorpb.DescriptorProto{
					Name: proto.String("M"),
					Field: []*descriptorpb.FieldDescriptorProto{{
						Name:     proto.String("g"),
						Number:   proto.Int32(1),
						Label:    descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
						Type:     descriptorpb.FieldDescriptorProto_TYPE_GROUP.Enum(),
						TypeName: proto.String(".t.M.G"),
					}},
					NestedType: []*descriptorpb.DescriptorProto{{Name: proto.String("G")}},
				})
				fd.Syntax = proto.String("proto2")
				return fd
			}(),
			want: "group fields are not supported",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			fd, err := protodesc.NewFile(tc.fd, nil)
			if err != nil {
				t.Fatal(err)
			}
			_, _, err = gogen.Generate(fd, gogen.Options{})
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("err = %v, want %q", err, tc.want)
			}
		})
	}
}

func TestRuntimeImportOption(t *testing.T) {
	req, err := testutil.LoadRequest(filepath.Join(testutil.RepoRoot(),
		"bindings", "go", "plugin", "testdata", "matrix3", "test.dump"))
	if err != nil {
		t.Fatal(err)
	}
	ctx, err := core.NewRequestContext(req)
	if err != nil {
		t.Fatal(err)
	}
	_, got, err := gogen.Generate(ctx.TargetDescriptors[0], gogen.Options{
		RuntimeImport: "example.com/pbwire/runtime",
	})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(got, `pbwire "example.com/pbwire/runtime"`) {
		t.Fatal("runtime import option not applied")
	}
}

func TestFieldNameCollisions(t *testing.T) {
	fd, err := protodesc.NewFile(file("names.proto", &descriptorpb.DescriptorProto{
		Name: proto.String("M"),
		Field: []*descriptorpb.FieldDescriptorProto{
			field("size", 1, descriptorpb.FieldDescriptorProto_TYPE_INT32),
			field("marshal", 2, descriptorpb.FieldDescriptorProto_TYPE_INT32),
			field("marshal_append", 3, descriptorpb.FieldDescriptorProto_TYPE_INT32),
			field("unmarshal", 4, descriptorpb.FieldDescriptorProto_TYPE_INT32),
			field("size_", 5, descriptorpb.FieldDescriptorProto_TYPE_INT32),
		},
	}), nil)
	if err != nil {
		t.Fatal(err)
	}
	_, got, err := gogen.Generate(fd, gogen.Options{})
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Size_", "Marshal_", "MarshalAppend_", "Unmarshal_", "Size__"} {
		re := regexp.MustCompile(`(?m)^\t` + name + `\s+int32$`)
		if !re.MatchString(got) {
			t.Fatalf("generated struct lacks field %s:\n%s", name, got)
		}
	}
}

func field(name string, number int32, kind descriptorpb.FieldDescriptorProto_Type) *descriptorpb.FieldDescriptorProto {
	return &descriptorpb.FieldDescriptorProto{
		Name:   proto.String(name),
		Number: proto.Int32(number),
		Label:  descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
		Type:   kind.Enum(),
	}
}

func file(name string, msgs ...*descriptorpb.DescriptorProto) *descriptorpb.FileDescriptorProto {
	return &descriptorpb.FileDescriptorProto{
		Name:        proto.String(name),
		Package:     proto.String("t"),
		Syntax:      proto.String("proto3"),
		MessageType: msgs,
	}
}
