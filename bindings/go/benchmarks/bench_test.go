// Benchmark the cgo binding on the four standard payload tiers.
//
// The Go binding covers scalar fields, so the tiers scale the *number of
// scalar fields* (4 / 16 / 64 / 256) instead of a repeated field. Every
// iteration crosses cgo twice (build the message, then encode), which is the
// cost the binding has to be judged on: run with
//
//	go test -bench . -benchmem ./benchmarks
package benchmarks

import (
	"fmt"
	"testing"

	pbwire "pbwire/runtime"
)

var tiers = []struct {
	label  string
	fields int
}{
	{"small", 4}, {"medium", 16}, {"large", 64}, {"xlarge", 256},
}

func schema(tb testing.TB, n int) (*pbwire.Schema, *pbwire.Message) {
	tb.Helper()
	s := pbwire.New()
	fields := make([]pbwire.Field, n)
	for i := range fields {
		fields[i] = pbwire.Field{Name: fmt.Sprintf("f%d", i), Number: uint32(i + 1), Type: pbwire.TInt32}
	}
	m, err := s.Message("Msg", fields)
	if err != nil {
		tb.Fatal(err)
	}
	return s, m
}

func values(n int) map[uint32]pbwire.Value {
	v := make(map[uint32]pbwire.Value, n)
	for i := 0; i < n; i++ {
		v[uint32(i+1)] = pbwire.Value{Uint: uint64(i * 1000)}
	}
	return v
}

func BenchmarkEncode(b *testing.B) {
	for _, tier := range tiers {
		s, m := schema(b, tier.fields)
		v := values(tier.fields)
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
		s, m := schema(b, tier.fields)
		wire, err := m.Encode(values(tier.fields))
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
