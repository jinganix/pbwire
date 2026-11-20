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
					{Name: "s", Lower: "s", Pascal: "S", Number: 2, Kind: protoreflect.StringKind},
				},
			},
		},
	}
}

// goBranchFile exercises every codec branch the emitter must mirror: nested
// messages (local and foreign), all presence rules, oneof (plain, message
// and synthetic-free proto3 optional), packed and unpacked repeated,
// maps-as-entry-structs, enums (unknown values kept as int), and skipped
// fields.
func goBranchFile() *model.File {
	return &model.File{
		Path:    "b.proto",
		Package: "b",
		Prefix:  "B",
		Imports: []model.Import{{Path: "xdep.proto", Package: "xdep", Prefix: "Xdep"}},
		Enums: []model.Enum{
			{Name: "E", Full: "b.E", Values: []model.EnumValue{{Name: "A", Number: 0}, {Name: "B", Number: 1}}},
		},
		Messages: []model.Message{
			{
				Name: "Inner",
				Full: "b.Inner",
				Fields: []model.Field{
					{Name: "v", Lower: "v", Pascal: "V", Number: 1, Kind: protoreflect.Int32Kind},
				},
			},
			{
				Name: "B_ByKeyEntry",
				Full: "b.B.ByKeyEntry",
				Fields: []model.Field{
					{Name: "key", Lower: "key", Pascal: "Key", Number: 1, Kind: protoreflect.StringKind},
					{Name: "value", Lower: "value", Pascal: "Value", Number: 2, Kind: protoreflect.Int32Kind},
				},
			},
			{
				Name: "B",
				Full: "b.B",
				Fields: []model.Field{
					{Name: "req", Lower: "req", Pascal: "Req", Number: 1, Kind: protoreflect.Int32Kind, Required: true},
					{Name: "opt", Lower: "opt", Pascal: "Opt", Number: 2, Kind: protoreflect.StringKind, NeedsHas: true},
					{Name: "p3opt", Lower: "p3opt", Pascal: "P3opt", Number: 3, Kind: protoreflect.Int64Kind, NeedsHas: true, Proto3Optional: true},
					{Name: "sub", Lower: "sub", Pascal: "Sub", Number: 4, Kind: protoreflect.MessageKind, Message: "Inner"},
					{Name: "ext", Lower: "ext", Pascal: "Ext", Number: 5, Kind: protoreflect.MessageKind, Message: "DepMsg", Import: "xdep.proto"},
					{Name: "packed", Lower: "packed", Pascal: "Packed", Number: 6, Kind: protoreflect.Int32Kind, Repeated: true, Packed: true},
					{Name: "unpacked", Lower: "unpacked", Pascal: "Unpacked", Number: 7, Kind: protoreflect.Int32Kind, Repeated: true},
					{Name: "tags", Lower: "tags", Pascal: "Tags", Number: 8, Kind: protoreflect.StringKind, Repeated: true},
					{Name: "subs", Lower: "subs", Pascal: "Subs", Number: 9, Kind: protoreflect.MessageKind, Message: "Inner", Repeated: true},
					{Name: "color", Lower: "color", Pascal: "Color", Number: 10, Kind: protoreflect.EnumKind, Enum: "E"},
					{Name: "by_key", Lower: "byKey", Pascal: "ByKey", Number: 11, Kind: protoreflect.MessageKind, Message: "B_ByKeyEntry", Repeated: true},
					{Name: "n", Lower: "n", Pascal: "N", Number: 12, Kind: protoreflect.Int32Kind, Oneof: "kind"},
					{Name: "t", Lower: "t", Pascal: "T", Number: 13, Kind: protoreflect.StringKind, Oneof: "kind"},
					{Name: "in", Lower: "in", Pascal: "In", Number: 14, Kind: protoreflect.MessageKind, Message: "Inner", Oneof: "kind"},
					{Name: "gone", Lower: "gone", Pascal: "Gone", Number: 15, Kind: protoreflect.MessageKind, Skip: "unresolved message type: use the dynamic API"},
					{Name: "f", Lower: "f", Pascal: "F", Number: 16, Kind: protoreflect.FloatKind},
					{Name: "d", Lower: "d", Pascal: "D", Number: 17, Kind: protoreflect.DoubleKind},
					{Name: "flag", Lower: "flag", Pascal: "Flag", Number: 18, Kind: protoreflect.BoolKind},
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

// Default: one thin model file per .proto. Structs, getters, field rows, and
// one-line forwarding to codec; no descriptor, no plan gate, no encoder state.
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
		t.Fatalf("missing model file, got %v", keys(got))
	}
	if strings.Contains(models, "_DESC_B64") {
		t.Error("model file must not embed the descriptor")
	}
	for _, want := range []string{
		`"pbwire/pbench/dyngo/codec"`,
		"func (m *M) Marshal() ([]byte, error)",
		"codec.EncodeModel(m, MMeta)",
		"func (m *M) Unmarshal(wire []byte) error",
		"codec.DecodeModel(wire, m, MMeta)",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("model file misses %q", want)
		}
	}
	meta, ok := got["t_pbwire_meta.go"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{
		"var MMeta = codec.NewTable(\"t.M\"",
		"Name: \"v\"",
		"Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32",
		"Num: 2, Wire: codec.WireLen, Kind: codec.KindString",
		"GetI32:",
		"SetStr:",
	} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
	for _, bad := range []string{
		"codec.NewTable",
		"getPlan",
		"func Init()",
		"UseSchema",
		"Preload",
		"sync/atomic",
		"switch num",
		"marshalM(",
		"unmarshalM(",
		"e.Tag(",
		"d.Varint",
		`"math"`,
	} {
		if strings.Contains(models, bad) {
			t.Errorf("model file must stay thin: %q", bad)
		}
	}
	if strings.Contains(models, "pbwire/pbench/dync/dynamic") {
		t.Error("model file must not import the dynamic API")
	}
}

// model=false: nothing to emit, since there is no separate schema half any more.
func TestGoFilesNoModel(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: false})
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 0 {
		t.Fatalf("want no files, got %v", files)
	}
}

// A dep file named after a Go keyword would emit `package struct` and fail
// gofmt; goIdent suffixes it, and the import alias goes through the same
// function so the two stay in sync.
func TestGoIdent(t *testing.T) {
	cases := map[string]string{
		"struct": "struct_pbwire",
		"type":   "type_pbwire",
		"go":     "go_pbwire",
		"xfile":  "xfile",
		"a-b":    "a_b",
		"":       "gen",
	}
	for in, want := range cases {
		if got := goIdent(in); got != want {
			t.Errorf("goIdent(%q) = %q, want %q", in, got, want)
		}
	}
}

// Branches: every model shape the dynamic emitter handled must have a codec
// counterpart. The branch file pins the key substrings of each shape.
func TestGoCodecBranches(t *testing.T) {
	files, err := goFiles(goBranchFile(), Options{
		Lang: "go", Model: true, Register: true, Registry: true,
		Mappings: map[string]string{"xdep.proto": "pbwire/tests/gen/xdep"},
	})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	models, ok := got["b_pbwire.go"]
	if !ok {
		t.Fatalf("missing model file, got %v", keys(got))
	}
	meta, ok := got["b_pbwire_meta.go"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	// The model side: types, getters, forwarding. No table, no wire logic.
	for _, want := range []string{
		"type B struct",
		"func (m *B) GetReq() int32",
		"Opt      *string",
		"func (m *B) Marshal() ([]byte, error)",
		"codec.EncodeModel(m, BMeta)",
		"codec.DecodeModel(wire, m, BMeta)",
		"type isB_kind interface",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("branch model file misses %q", want)
		}
	}
	for _, bad := range []string{"codec.NewTable", "GetI32:", "SetStr:", "Sub: func()"} {
		if strings.Contains(models, bad) {
			t.Errorf("branch model file must stay free of field tables: %q", bad)
		}
	}
	// The meta side: every field-fact and closure shape.
	for _, want := range []string{
		// required: always written, no presence guard.
		"Num: 1, Wire: codec.WireVarint, Kind: codec.KindInt32, Required: true",
		// proto2 optional pointer: presence through the pointer.
		"GetOptStr:",
		"SetOptStr:",
		// proto3 optional int64.
		"GetOptI64:",
		// local submessage: sub-table reference plus message hooks.
		"Sub: func() *codec.Table { return InnerMeta }",
		"GetOrCreateMsg:",
		// foreign submessage: bytes through the sibling package.
		"Foreign: &codec.ForeignOps",
		"xdep.DepMsg",
		// packed vs unpacked repeated.
		"Num: 6, Wire: codec.WireVarint, Kind: codec.KindInt32, Repeated: true, Packed: true",
		"Num: 7, Wire: codec.WireVarint, Kind: codec.KindInt32, Repeated: true,",
		// repeated string rows carry slice hooks, not tags.
		"GetRepStr:",
		"AppendStr:",
		// enum preserved as int (no validation).
		"Kind: codec.KindEnum",
		"= E(v)",
		// maps ride as repeated entry structs.
		"B_ByKeyEntryMeta",
		"RepMsgLen:",
		// oneof members key on the wrapper, in field-number order.
		"GetOneof:",
		"SetOneof:",
		".Kind = &B_N{N: v.(int32)}",
		".Kind = &B_T{T: v.(string)}",
		"return &Inner{}", // oneof message member allocator
		// float/double need no math import: bit casts live in the engine.
		"Kind: codec.KindFloat",
		"Kind: codec.KindDouble",
		// field-number order is preserved in the table.
		"var BMeta = codec.NewTable(\"b.B\"",
	} {
		if !strings.Contains(meta, want) {
			t.Errorf("branch meta file misses %q", want)
		}
	}
	for _, bad := range []string{
		"marshalB",
		"unmarshalB",
		"switch num",
		"e.Tag(",
		"d.Skip(num, wt, depth)",
		`"math"`,
		"GetInt(",
		"SetInt(",
		"pbwire/pbench/dync/dynamic",
		"type B struct",
	} {
		if strings.Contains(meta, bad) {
			t.Errorf("branch meta file must not contain %q", bad)
		}
	}
	// Rows land in field-number order: req=1 before opt=2.
	req, opt := strings.Index(meta, "Num: 1,"), strings.Index(meta, "Num: 2,")
	if req < 0 || opt < 0 || req > opt {
		t.Error("table rows must run in field-number order")
	}
}
