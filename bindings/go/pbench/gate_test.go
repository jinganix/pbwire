//go:build pbench

package pbench

import (
	"fmt"
	"math"
	"testing"

	"google.golang.org/protobuf/proto"
	"pbwire/pbench/cshim"
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
	cDynGo, err := dyngo.New()
	if err != nil {
		t.Fatal(err)
	}
	cShim, err := cshim.New()
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
		{"cshim", cShim.Marshal, cShim.Unmarshal},
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
	_ = math.MinInt32
	return m
}
