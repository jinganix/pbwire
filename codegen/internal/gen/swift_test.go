package gen

import (
	"strings"
	"testing"

	"pbwire-codegen/internal/model"

	"google.golang.org/protobuf/reflect/protoreflect"
)

func swiftTestFile() *model.File {
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

// Default: models plus the schema file with the lazy schema and the list.
func TestSwiftFilesDefault(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 2 {
		t.Fatalf("want models+meta files, got %v", keys(got))
	}
	models, ok := got["matrix3.swift"]
	if !ok {
		t.Fatalf("missing models file, got %v", keys(got))
	}
	if strings.Contains(models, "descData") || strings.Contains(models, "Data(base64Encoded") {
		t.Error("models file must not embed the descriptor")
	}
	if !strings.Contains(models, "matrix3_schema.message") {
		t.Error("models resolve through the stem-prefixed schema")
	}
	meta, ok := got["matrix3_meta.swift"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{"_matrix3_embedded", "func matrix3_initSchema", "func matrix3_useSchema", "var matrix3_schema", "matrix3_messageNames", "func matrix3_preload"} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
}

// model=false leaves the schema file for dynamic-only users.
func TestSwiftFilesNoModel(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: false, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Name] = f.Content
	}
	if len(got) != 1 {
		t.Fatalf("want meta file only, got %v", keys(got))
	}
	if _, ok := got["matrix3_meta.swift"]; !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
}

// register=false drops the implicit load; initSchema or useSchema first.
func TestSwiftFilesManualRegister(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: false, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.swift" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "_matrix3_embedded") {
		t.Error("register=false must drop the embedded lazy schema")
	}
	if !strings.Contains(meta, "call matrix3_initSchema() first") {
		t.Error("register=false must fail loudly")
	}
}

// registry=false drops the name list and preload; the loader stays.
func TestSwiftFilesNoRegistry(t *testing.T) {
	files, err := swiftFiles(swiftTestFile(), Options{Lang: "swift", Model: true, Register: true, Registry: false})
	if err != nil {
		t.Fatal(err)
	}
	var meta string
	for _, f := range files {
		if f.Name == "matrix3_meta.swift" {
			meta = f.Content
		}
	}
	if strings.Contains(meta, "messageNames") || strings.Contains(meta, "preload") {
		t.Error("registry=false must drop the list")
	}
}

// swiftLayoutTestFile is the layout gate's fixture: scalars of every width, a
// string, a repeated message and a proto3 optional (a has flag next to a
// pointer-shaped absence).
func swiftLayoutTestFile() *model.File {
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
					{Name: "flag", Lower: "flag", Pascal: "Flag", Number: 13, Kind: protoreflect.BoolKind},
					{Name: "str", Lower: "str", Pascal: "Str", Number: 14, Kind: protoreflect.StringKind},
					{Name: "inner", Lower: "inner", Pascal: "Inner", Number: 16, Kind: protoreflect.MessageKind, Message: "Inner3", Full: "structtest.Inner3"},
					{Name: "nums", Lower: "nums", Pascal: "Nums", Number: 17, Kind: protoreflect.Int32Kind, Repeated: true, Packed: true},
					{Name: "opt_i32", Lower: "optI32", Pascal: "OptI32", Number: 20, Kind: protoreflect.Int32Kind, NeedsHas: true},
				},
			},
		},
	}
}

// Layout gate: the Swift mirror is a @frozen value type (no references, so no
// ARC traffic per field) whose slot offsets are the ones the C layout codec is
// handed, computed by MemoryLayout at run time instead of assumed.
func TestSwiftLayout(t *testing.T) {
	files, err := swiftFiles(swiftLayoutTestFile(), Options{Lang: "swift", Model: true, Register: true, Registry: true, DescSetB64: "e30="})
	if err != nil {
		t.Fatal(err)
	}
	var models string
	for _, f := range files {
		if f.Name == "matrix3.swift" {
			models = f.Content
		}
	}
	if models == "" {
		t.Fatal("missing models file")
	}
	for _, want := range []string{
		"@frozen\npublic struct Structtest_Scalars3Repr: Pbwire.ReprMessage {",
		`public static let protoName = "structtest.Scalars3"`,
		"public static let fields: [Pbwire.FieldPlan] = [",
		"public static let children: [any Pbwire.ReprMessage.Type] = [Structtest_Inner3Repr.self]",
		"public var str: UnsafePointer<CChar>? = nil",
		"public var inner: UnsafeMutablePointer<Structtest_Inner3Repr>? = nil",
		"public var nums: UnsafeMutablePointer<Int32>? = nil",
		"public var countNums: Int = 0",
		"public var hasOptI32: Int32 = 0",
		"Pbwire.FieldPlan(number: 1, kind: .int32, offset: MemoryLayout<Structtest_Scalars3Repr>.offset(of: \\.i32)!, type: 5, wire: 0)",
		"Pbwire.FieldPlan(number: 17, kind: .int32, offset: MemoryLayout<Structtest_Scalars3Repr>.offset(of: \\.nums)!, type: 5, wire: 0, repeated: true, count: MemoryLayout<Structtest_Scalars3Repr>.offset(of: \\.countNums)!, packed: true)",
		"Pbwire.FieldPlan(number: 20, kind: .int32, offset: MemoryLayout<Structtest_Scalars3Repr>.offset(of: \\.optI32)!, type: 5, wire: 0, has: MemoryLayout<Structtest_Scalars3Repr>.offset(of: \\.hasOptI32)!)",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("models file misses %q", want)
		}
	}
	// The mirror holds no references (no ARC per field): every slot is a
	// pointer, a fixed-width scalar or the core's inline bytes value.
	repr := models[strings.Index(models, "struct Structtest_Scalars3Repr"):]
	if i := strings.Index(repr, "public static let protoName"); i >= 0 {
		repr = repr[:i]
	}
	for _, banned := range []string{"String =", "[Int64]", "Data"} {
		if strings.Contains(repr, banned) {
			t.Errorf("mirror must not hold %q (ARC per field)", banned)
		}
	}
}
