//go:build pbench

package pbench

import (
	"bytes"
	"fmt"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"pbwire/pbench/dync"
	"pbwire/pbench/dyngo"
	"pbwire/tests/gen/matrix3"
	"pbwire/tests/gen_oracle/matrix3pb"
)

// Every dynamic arm runs the same user-visible task: native struct in, native
// struct out. The gate checks both directions against protobuf-go before any
// timing happens.
func TestArmsGate(t *testing.T) {
	cDync, err := dync.New()
	if err != nil {
		t.Fatal(err)
	}
	defer cDync.Close()
	cDynGo, err := dyngo.New()
	if err != nil {
		t.Fatal(err)
	}
	arms := []struct {
		name    string
		marshal func(*matrix3.Scalars3) ([]byte, error)
		decode  func([]byte) (*matrix3.Scalars3, error)
	}{
		{"dync", cDync.Marshal, cDync.Unmarshal},
		{"dyngo", cDynGo.Marshal, cDynGo.Unmarshal},
		{"dyngo-direct", cDynGo.DirectMarshal, cDynGo.DirectUnmarshal},
	}
	for _, arm := range arms {
		for _, tr := range []struct{ n int }{{0}, {1}, {100}, {3000}} {
			want := fill(tr.n)
			ref, err := proto.Marshal(want)
			if err != nil {
				t.Fatal(err)
			}
			got, err := arm.marshal(structOf(want))
			if err != nil {
				t.Fatalf("%s x%d: marshal: %v", arm.name, tr.n, err)
			}
			if string(got) != string(ref) {
				t.Fatalf("%s x%d: encode differs from protobuf-go (%d vs %d bytes)",
					arm.name, tr.n, len(got), len(ref))
			}
			back, err := arm.decode(ref)
			if err != nil {
				t.Fatalf("%s x%d: decode: %v", arm.name, tr.n, err)
			}
			if err := checkStruct(fmt.Sprintf("%s x%d", arm.name, tr.n), back, want); err != nil {
				t.Fatal(err)
			}
		}
	}
}

// structOf copies the official payload into the pbwire model the arms take.
func structOf(w *matrix3pb.Scalars3) *matrix3.Scalars3 {
	m := &matrix3.Scalars3{
		I32: w.I32, I64: w.I64, U32: w.U32, U64: w.U64, S32: w.S32, S64: w.S64,
		Fx32: w.Fx32, Fx64: w.Fx64, Sfx32: w.Sfx32, Sfx64: w.Sfx64,
		F32: w.F32, F64: w.F64, Flag: w.Flag, Str: w.Str, Color: matrix3.Color3(w.Color),
		OptI32: w.OptI32, OptStr: w.OptStr,
	}
	if w.Inner != nil {
		m.Inner = &matrix3.Inner3{Name: w.Inner.Name, Value: w.Inner.Value}
	}
	for _, v := range w.Nums {
		m.Nums = append(m.Nums, v)
	}
	m.Tags = append(m.Tags, w.Tags...)
	for _, v := range w.Inners {
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: v.Name, Value: v.Value})
	}
	m.Flags = append(m.Flags, w.Flags...)
	return m
}

// TestGateSemantics pins the two semantic vectors from issue 41: invalid
// UTF-8 in a proto3 string must fail decode on every path, and unknown
// field 999 must round-trip byte-identical through the oracle and through
// the thin model (retained in the generated struct's unknownFields, cleared
// by Reset, replayed after the known fields like the reference).
func TestGateSemantics(t *testing.T) {
	// Vector 1: str is field 14 (BytesType). Valid UTF-8 decodes; the
	// same shape with invalid bytes must error mentioning UTF-8.
	good := protowire.AppendTag(nil, 14, protowire.BytesType)
	good = protowire.AppendBytes(good, []byte("hello"))
	if err := new(matrix3.Scalars3).Unmarshal(good); err != nil {
		t.Fatalf("valid UTF-8 rejected: %v", err)
	}
	bad := protowire.AppendTag(nil, 14, protowire.BytesType)
	bad = protowire.AppendBytes(bad, []byte{0xff, 0xfe, 'x'})
	if err := new(matrix3.Scalars3).Unmarshal(bad); err == nil {
		t.Fatal("thin model decoded invalid UTF-8 without error")
	} else if !strings.Contains(strings.ToLower(err.Error()), "utf") {
		t.Fatalf("thin model error does not mention UTF-8: %v", err)
	}

	cDync, err := dync.New()
	if err != nil {
		t.Fatal(err)
	}
	defer cDync.Close()
	cDynGo, err := dyngo.New()
	if err != nil {
		t.Fatal(err)
	}
	for name, decode := range map[string]func([]byte) (*matrix3.Scalars3, error){
		"dync":  cDync.Unmarshal,
		"dyngo": cDynGo.Unmarshal,
	} {
		if _, err := decode(good); err != nil {
			t.Fatalf("%s rejected valid UTF-8: %v", name, err)
		}
		if _, err := decode(bad); err == nil {
			t.Fatalf("%s decoded invalid UTF-8 without error", name)
		}
	}

	// Vector 2: unknown field 999 (varint 7). The oracle keeps it, and so
	// must the thin model, byte for byte; Reset clears the retained bytes.
	unk := protowire.AppendTag(nil, 999, protowire.VarintType)
	unk = protowire.AppendVarint(unk, 7)
	var o matrix3pb.Scalars3
	if err := proto.Unmarshal(unk, &o); err != nil {
		t.Fatal(err)
	}
	if bout, err := proto.Marshal(&o); err != nil || !bytes.Equal(bout, unk) {
		t.Fatalf("oracle round trip: %x, err %v", bout, err)
	}
	var m matrix3.Scalars3
	if err := m.Unmarshal(unk); err != nil {
		t.Fatalf("thin model rejected unknown-only wire: %v", err)
	}
	if again, err := m.Marshal(); err != nil || !bytes.Equal(again, unk) {
		t.Fatalf("thin model must retain unknowns: got %x want %x, err %v", again, unk, err)
	}
	m.Reset()
	if again, err := m.Marshal(); err != nil || len(again) != 0 {
		t.Fatalf("Reset must clear retained unknowns: got %x, err %v", again, err)
	}

	// Vector 3: field 999 nested inside Inner3 (field 16) round-trips
	// untouched, inside the submessage's length prefix like the reference.
	inner := protowire.AppendTag(nil, 999, protowire.VarintType)
	inner = protowire.AppendVarint(inner, 7)
	nested := protowire.AppendTag(nil, 16, protowire.BytesType)
	nested = protowire.AppendBytes(nested, inner)
	var m2 matrix3.Scalars3
	if err := m2.Unmarshal(nested); err != nil {
		t.Fatalf("thin model rejected nested unknown wire: %v", err)
	}
	if m2.Inner == nil {
		t.Fatal("thin model dropped the known inner message carrying unknowns")
	}
	if again, err := m2.Marshal(); err != nil || !bytes.Equal(again, nested) {
		t.Fatalf("thin model must replay nested unknowns: got %x want %x, err %v", again, nested, err)
	}

	// Vector 4: the dynamic arms retain unknowns end to end like the oracle.
	// The converters move them through the meta hooks on every level, so an
	// unknown-carrying wire round-trips through tree and struct alike.
	arms := []struct {
		name    string
		marshal func(*matrix3.Scalars3) ([]byte, error)
		decode  func([]byte) (*matrix3.Scalars3, error)
	}{
		{"dync", cDync.Marshal, cDync.Unmarshal},
		{"dyngo", cDynGo.Marshal, cDynGo.Unmarshal},
	}
	for _, arm := range arms {
		back, err := arm.decode(unk)
		if err != nil {
			t.Fatalf("%s rejected unknown-only wire: %v", arm.name, err)
		}
		if again, err := arm.marshal(back); err != nil || !bytes.Equal(again, unk) {
			t.Fatalf("%s must retain unknowns: got %x want %x, err %v", arm.name, again, unk, err)
		}
		nback, err := arm.decode(nested)
		if err != nil {
			t.Fatalf("%s rejected nested unknown wire: %v", arm.name, err)
		}
		if nagain, err := arm.marshal(nback); err != nil || !bytes.Equal(nagain, nested) {
			t.Fatalf("%s must replay nested unknowns: got %x want %x, err %v", arm.name, nagain, nested, err)
		}
	}
}
