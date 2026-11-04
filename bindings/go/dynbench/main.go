// Bench for the dynamic MVP: same matrix3 tiers as the static bench,
// so numbers juxtapose directly. Usage (from bindings/go):
//
//	protoc -I../c/tests/data --descriptor_set_out=/tmp/matrix3.desc \
//	    --include_imports ../c/tests/data/matrix3.proto
//	go run ./dynbench -desc /tmp/matrix3.desc
package main

import (
	"flag"
	"fmt"
	"math"
	"os"
	"testing"

	"pbwire/dynamic"
)

func mustSet(m *dynamic.Message, err error) *dynamic.Message {
	if err != nil {
		panic(err)
	}
	return m
}

func fill(typ *dynamic.MessageType, n int) *dynamic.Message {
	m := typ.New()
	chk := func(err error) {
		if err != nil {
			panic(err)
		}
	}
	chk(m.SetInt("i32", math.MinInt32))
	chk(m.SetInt("i64", math.MaxInt64))
	chk(m.SetUint("u32", 0xffffffff))
	chk(m.SetUint("u64", 1<<64-1))
	chk(m.SetInt("s32", math.MinInt32))
	chk(m.SetInt("s64", math.MinInt64))
	chk(m.SetUint("fx32", 0xdeadbeef))
	chk(m.SetUint("fx64", 72623859790382856))
	chk(m.SetInt("sfx32", math.MinInt32))
	chk(m.SetInt("sfx64", math.MinInt64))
	chk(m.SetFloat("f32", float32(math.Inf(1))))
	chk(m.SetDouble("f64", -123456.789))
	chk(m.SetBool("flag", true))
	chk(m.SetString("str", "hello"))
	chk(m.SetInt("color", 2))
	chk(m.SetInt("opt_i32", -7))
	chk(m.SetString("opt_str", "opt"))
	inner, err := m.NewMessage("inner")
	chk(err)
	chk(inner.SetString("name", "kid"))
	chk(inner.SetInt("value", 42))
	for i := 0; i < n; i++ {
		chk(m.SetInt("nums", int64(i-3)))
		chk(m.SetString("tags", "tag"))
		chk(m.SetBool("flags", i%2 == 0))
		sub, err := m.NewMessage("inners")
		chk(err)
		chk(sub.SetString("name", "grandkid"))
		chk(sub.SetInt("value", int64(i+1)))
	}
	return m
}

func main() {
	descPath := flag.String("desc", "", "matrix3.desc from protoc")
	flag.Parse()
	if *descPath == "" {
		fmt.Fprintln(os.Stderr, "usage: dynbench -desc <matrix3.desc>")
		os.Exit(2)
	}
	desc, err := os.ReadFile(*descPath)
	if err != nil {
		panic(err)
	}

	// Hot-swap cost first: full load+link+swap of a 74913-byte schema.
	r, err := dynamic.NewRegistry(desc)
	if err != nil {
		panic(err)
	}
	swapR := testing.Benchmark(func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			s, err := r.Swap(desc)
			if err != nil {
				b.Fatal(err)
			}
			_ = s
		}
		b.StopTimer()
		r.Purge()
	})
	fmt.Printf("swap (load+link): %12.1f ns/op\n\n", float64(swapR.NsPerOp()))

	typ, err := r.Current().Message("structtest.Scalars3")
	if err != nil {
		panic(err)
	}
	defer typ.Close()

	tiers := []struct {
		label   string
		repeats int
		want    int
	}{{"small", 0, 136}, {"medium", 1, 174}, {"large", 100, 2469}, {"xlarge", 3000, 74913}}
	fmt.Printf("%-7s %-6s %12s %10s  (static encode/decode for scale)\n", "tier", "op", "ns/op", "MB/s")
	for _, tier := range tiers {
		m := fill(typ, tier.repeats)
		wire, err := m.Encode()
		if err != nil {
			panic(err)
		}
		if len(wire) != tier.want {
			panic(fmt.Sprintf("%s: %d bytes, want %d", tier.label, len(wire), tier.want))
		}
		enc := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				out, err := m.Encode()
				if err != nil {
					b.Fatal(err)
				}
				_ = out
			}
		})
		dec := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				back, err := typ.Decode(wire)
				if err != nil {
					b.Fatal(err)
				}
				back.Close()
			}
		})
		fmt.Printf("%-7s %-6s %12.1f %10.1f\n", tier.label, "encode",
			float64(enc.NsPerOp()), float64(tier.want)/float64(enc.NsPerOp())*1000)
		fmt.Printf("%-7s %-6s %12.1f %10.1f\n", tier.label, "decode",
			float64(dec.NsPerOp()), float64(tier.want)/float64(dec.NsPerOp())*1000)
		m.Close()
	}
	r.Purge()
}
