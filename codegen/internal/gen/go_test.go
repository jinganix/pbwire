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

// UTF8 is a proto3-only column: the engine rejects invalid UTF-8 on decode
// only when the row says so (plan.go), and only proto3 strings say so.
func TestGoTableUTF8Proto3Only(t *testing.T) {
	meta := func(syntax string) string {
		t.Helper()
		f := goTestFile()
		f.Syntax = syntax
		files, err := goFiles(f, Options{Lang: "go", Model: true, Register: true, Registry: true})
		if err != nil {
			t.Fatal(err)
		}
		got := goFileMap(t, files)
		m, ok := got["t_pbwire_meta.go"]
		if !ok {
			t.Fatalf("missing meta file, got %v", keys(got))
		}
		return m
	}
	if m := meta("proto3"); !strings.Contains(m, "UTF8: true") {
		t.Error("proto3 string row must carry UTF8: true")
	}
	for _, syntax := range []string{"", "proto2"} {
		if m := meta(syntax); strings.Contains(m, "UTF8") {
			t.Errorf("syntax %q must not emit UTF8", syntax)
		}
	}
}

// Default: one thin model file per .proto. Structs, getters and Reset; the
// field rows live in meta, the Marshal/Unmarshal forwarding in the cshim
// wiring file. No descriptor, no plan gate, no encoder state.
func TestGoFilesDefault(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want model+meta files, got %v", keys(got))
	}
	if _, ok := got["t_pbwire_cshim.go"]; ok {
		t.Fatal("codegen must not emit cshim wiring anymore")
	}
	models, ok := got["t_pbwire.go"]
	if !ok {
		t.Fatalf("missing model file, got %v", keys(got))
	}
	if strings.Contains(models, "_DESC_B64") {
		t.Error("model file must not embed the descriptor")
	}
	for _, want := range []string{
		"func (m *M) Reset()",
		"func (m *M) GetV() int32",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("model file misses %q", want)
		}
	}
	for _, want := range []string{
		"func (m *M) Marshal() ([]byte, error)",
		"return runtime.EncodeModel(m, MMeta)",
		"func (m *M) Unmarshal(wire []byte) error",
		"return runtime.DecodeModel(wire, m, MMeta)",
		`"pbwire/runtime"`,
	} {
		if !strings.Contains(models, want) {
			t.Errorf("model file must forward to the table engine, misses %q", want)
		}
	}
	meta, ok := got["t_pbwire_meta.go"]
	if !ok {
		t.Fatalf("missing meta file, got %v", keys(got))
	}
	for _, want := range []string{
		"var MMeta = runtime.NewTable(\"t.M\"",
		"Name: \"v\"",
		"Num: 1, Wire: runtime.WireVarint, Kind: runtime.KindInt32",
		"Num: 2, Wire: runtime.WireLen, Kind: runtime.KindString",
		"GetI32:",
		"SetStr:",
		"Fast: true, Off: uint32(unsafe.Offsetof(M{}.V))",
		"Fast: true, Off: uint32(unsafe.Offsetof(M{}.S))",
		"\"unsafe\"",
	} {
		if !strings.Contains(meta, want) {
			t.Errorf("meta file misses %q", want)
		}
	}
	// Count/Elem are layout facts nothing reads; emitting them would imply
	// engine behavior that does not exist.
	for _, bad := range []string{"Elem:", "Count:"} {
		if strings.Contains(meta, bad) {
			t.Errorf("meta file must not emit dead columns: %q", bad)
		}
	}
	for _, bad := range []string{
		"runtime.NewTable",
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
	// The model side: types, getters, Reset. No table, no wire logic,
	// no Marshal bodies.
	for _, want := range []string{
		"type B struct",
		"func (m *B) GetReq() int32",
		"Opt      *string",
		"func (m *B) Reset()",
		"type isB_kind interface",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("branch model file misses %q", want)
		}
	}
	for _, bad := range []string{"runtime.NewTable", "GetI32:", "SetStr:", "Sub: func()", "EncodeRaw"} {
		if strings.Contains(models, bad) {
			t.Errorf("branch model file must stay free of tables and runtime calls: %q", bad)
		}
	}
	// The branch fixture carries a skipped field, so the whole file stays
	// on the table engine: dyngo forwarding in the model, no cshim file.
	for _, want := range []string{
		"func (m *B) Marshal() ([]byte, error)",
		"runtime.EncodeModel(m, BMeta)",
		"runtime.DecodeModel(wire, m, BMeta)",
	} {
		if !strings.Contains(models, want) {
			t.Errorf("skipped file must stay on the table engine, misses %q", want)
		}
	}
	if _, ok := got["b_pbwire_cshim.go"]; ok {
		t.Error("skipped file must not emit a cshim file")
	}
	// The meta side: every field-fact and closure shape.
	for _, want := range []string{
		// required: always written, no presence guard.
		"Num: 1, Wire: runtime.WireVarint, Kind: runtime.KindInt32, Required: true",
		// proto2 optional pointer: presence through the pointer.
		"GetOptStr:",
		"SetOptStr:",
		// proto3 optional int64.
		"GetOptI64:",
		// local submessage: sub-table reference plus message hooks.
		"Sub: func() *runtime.Table { return InnerMeta }",
		"GetOrCreateMsg:",
		// foreign submessage: bytes through the sibling package.
		"Foreign: &runtime.ForeignOps",
		"xdep.DepMsg",
		// packed vs unpacked repeated.
		"Num: 6, Wire: runtime.WireVarint, Kind: runtime.KindInt32, Repeated: true, Packed: true",
		"Num: 7, Wire: runtime.WireVarint, Kind: runtime.KindInt32, Repeated: true,",
		// repeated string rows carry slice hooks, not tags.
		"GetRepStr:",
		"AppendStr:",
		// enum preserved as int (no validation).
		"Kind: runtime.KindEnum",
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
		"Kind: runtime.KindFloat",
		"Kind: runtime.KindDouble",
		// field-number order is preserved in the table.
		"var BMeta = runtime.NewTable(\"b.B\"",
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

// No cshim wiring is emitted anymore: every file ships model+meta over
// the shared table engine. Skips, foreign rows, oneof and groups stay
// covered by the fallback tests below end to end through goFiles.
func TestGoFilesNoCshim(t *testing.T) {
	files, err := goFiles(goTestFile(), Options{Lang: "go", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if len(got) != 2 {
		t.Fatalf("want model+meta files, got %v", keys(got))
	}
	if _, ok := got["b_pbwire_cshim.go"]; ok {
		t.Fatal("codegen must not emit cshim wiring anymore")
	}
	models := got["t_pbwire.go"]
	for _, want := range []string{"runtime.EncodeModel(m,", "runtime.DecodeModel(wire, m,"} {
		if !strings.Contains(models, want) {
			t.Errorf("model file must forward to the table engine, misses %q", want)
		}
	}
	for _, bad := range []string{"EncodeRaw", "DecodeInto", "CshimRegistry"} {
		if strings.Contains(models, bad) {
			t.Errorf("model file must stay thin, found %q", bad)
		}
	}
}

// A message field defined in another file must fail the cshim emission too:
// the file's C codec only knows its own registry, and a generated copy of a
// slice of foreign structs would not even compile. The model keeps the table
// engine's Marshal/Unmarshal instead.
func TestGoCshimForeignFieldFallsBack(t *testing.T) {
	files, err := goFiles(&model.File{
		Path: "u.proto", Package: "u", Prefix: "U",
		Imports: []model.Import{{Path: "xdep.proto", Package: "xdep", Prefix: "Xdep"}},
		Messages: []model.Message{{
			Name: "Uses", Full: "u.Uses",
			Fields: []model.Field{
				{Name: "one", Lower: "one", Pascal: "One", Number: 1, Kind: protoreflect.MessageKind, Message: "DepMsg", Import: "xdep.proto"},
				{Name: "many", Lower: "many", Pascal: "Many", Number: 2, Kind: protoreflect.MessageKind, Message: "DepMsg", Import: "xdep.proto", Repeated: true},
			},
		}},
	}, Options{Lang: "go", Model: true, Mappings: map[string]string{"xdep.proto": "pbwire/tests/gen/xdep"}})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if _, ok := got["u_pbwire_cshim.go"]; ok {
		t.Error("a foreign message field must not emit a cshim file")
	}
	models := got["u_pbwire.go"]
	for _, want := range []string{"runtime.EncodeModel(m, UsesMeta)", "runtime.DecodeModel(wire, m, UsesMeta)"} {
		if !strings.Contains(models, want) {
			t.Errorf("foreign model must keep the table engine, misses %q", want)
		}
	}
}

// A message named by another message's field cannot carry repeated rows: the
// C decode publishes repeat buffers for the root table only. The whole file
// then stays on the table engine, as addressbook's AddressBook -> Person
// does.
func TestGoCshimSubRepeatedFallsBack(t *testing.T) {
	files, err := goFiles(&model.File{
		Path: "a.proto", Package: "a", Prefix: "A",
		Messages: []model.Message{
			{Name: "Phone", Full: "a.Phone", Fields: []model.Field{
				{Name: "n", Lower: "n", Pascal: "N", Number: 1, Kind: protoreflect.StringKind},
			}},
			{Name: "Person", Full: "a.Person", Fields: []model.Field{
				{Name: "phones", Lower: "phones", Pascal: "Phones", Number: 1, Kind: protoreflect.MessageKind, Message: "Phone", Repeated: true},
			}},
			{Name: "Book", Full: "a.Book", Fields: []model.Field{
				{Name: "people", Lower: "people", Pascal: "People", Number: 1, Kind: protoreflect.MessageKind, Message: "Person", Repeated: true},
			}},
		},
	}, Options{Lang: "go", Model: true})
	if err != nil {
		t.Fatal(err)
	}
	got := goFileMap(t, files)
	if _, ok := got["a_pbwire_cshim.go"]; ok {
		t.Error("a sub table with repeated rows must not emit a cshim file")
	}
	models := got["a_pbwire.go"]
	for _, want := range []string{"runtime.EncodeModel(m, PhoneMeta)", "runtime.EncodeModel(m, BookMeta)"} {
		if !strings.Contains(models, want) {
			t.Errorf("nested-repeat file must keep the table engine, misses %q", want)
		}
	}
}

// A drivable file whose model needs no imports emits no empty import block:
// gofmt-clean output is part of the contract.
func TestGoFilesNoEmptyImport(t *testing.T) {
	files, err := goFiles(&model.File{
		Path: "n.proto", Package: "n", Prefix: "N",
		Messages: []model.Message{{
			Name: "M", Full: "n.M",
			Fields: []model.Field{{Name: "v", Lower: "v", Pascal: "V", Number: 1, Kind: protoreflect.Int32Kind}},
		}},
	}, Options{Lang: "go", Model: true, Register: true, Registry: true})
	if err != nil {
		t.Fatal(err)
	}
	if got := goFileMap(t, files)["n_pbwire.go"]; strings.Contains(got, "import ()") {
		t.Error("model file must not emit an empty import block")
	}
}
