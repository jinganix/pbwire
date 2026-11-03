// Benchmark the cgo binding on four payload sizes.
//
// The schema comes from matrix3.desc (structtest.Scalars3) and the tiers encode
// a growing subset of its scalar fields, so the numbers include the cgo
// crossings that dominate this binding. Run with
//
//	go test -bench . -benchmem ./benchmarks
package benchmarks

import (
	"path/filepath"
	"runtime"
	"testing"

	pbwire "pbwire/runtime"
)

var descPath = filepath.Join(sourceDir(), "..", "..", "c", "tests", "data", "matrix3.desc")

var tiers = []struct {
	label   string
	numbers []uint32
}{
	{"small", []uint32{1, 2, 3, 4}},
	{"medium", []uint32{1, 2, 3, 4, 5, 6, 7, 8}},
	{"large", []uint32{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
	{"xlarge", []uint32{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20}},
}

func setup(tb testing.TB) (*pbwire.Schema, *pbwire.Message) {
	tb.Helper()
	s, err := pbwire.LoadFile(descPath)
	if err != nil {
		tb.Fatal(err)
	}
	m, err := s.Message("structtest.Scalars3")
	if err != nil {
		tb.Fatal(err)
	}
	return s, m
}

func values(numbers []uint32, text bool) map[uint32]pbwire.Value {
	v := make(map[uint32]pbwire.Value, len(numbers))
	for i, n := range numbers {
		v[n] = pbwire.Value{Uint: uint64(i) * 1000}
	}
	if text {
		v[14] = pbwire.Value{Bytes: []byte("benchmark payload"), IsStr: true}
	}
	return v
}

func BenchmarkEncode(b *testing.B) {
	for _, tier := range tiers {
		s, m := setup(b)
		v := values(tier.numbers, true)
		wire, err := m.Encode(v)
		if err != nil {
			b.Fatal(err)
		}
		b.Run(tier.label, func(b *testing.B) {
			b.SetBytes(int64(len(wire)))
			for i := 0; i < b.N; i++ {
				if _, err := m.Encode(v); err != nil {
					b.Fatal(err)
				}
			}
		})
		s.Close()
	}
}

func BenchmarkDecode(b *testing.B) {
	for _, tier := range tiers {
		s, m := setup(b)
		wire, err := m.Encode(values(tier.numbers, true))
		if err != nil {
			b.Fatal(err)
		}
		b.Run(tier.label, func(b *testing.B) {
			b.SetBytes(int64(len(wire)))
			for i := 0; i < b.N; i++ {
				if _, err := m.Decode(wire); err != nil {
					b.Fatal(err)
				}
			}
		})
		s.Close()
	}
}

// sourceDir is this file's directory, so the descriptor path does not depend on
// the working directory Go picks for the package.
func sourceDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(file)
}
