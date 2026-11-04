// Unified bench: static vs official vs vtproto vs dynamic (cgo), same
// matrix3 tiers, one table in MB/s plus dynamic ratios. Run from
// bindings/go/benchmarks: go run ./unified (needs protoc for the dynamic
// descriptor set, same as make go-bench).
package main

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"google.golang.org/protobuf/proto"

	official "pbwire/benchmarks/gen/official"
	pbwire "pbwire/benchmarks/gen/pbwire"
	"pbwire/dynamic"
)

var tiers = []struct {
	label   string
	repeats int
}{
	{"small", 0},
	{"medium", 1},
	{"large", 100},
	{"xlarge", 3000},
}

func fatalf(format string, args ...any) {
	fmt.Printf("FAIL "+format+"\n", args...)
	os.Exit(1)
}

func ptr[T any](v T) *T { return &v }

func fillPB(n int) *pbwire.Scalars3 {
	m := &pbwire.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64,
		U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64,
		Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64,
		F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: pbwire.Color3_GREEN,
		Inner:  &pbwire.Inner3{Name: "kid", Value: 42},
		OptI32: ptr(int32(-7)), OptStr: ptr("opt"),
	}
	m.Nums = make([]int32, n)
	m.Tags = make([]string, n)
	m.Inners = make([]pbwire.Inner3, n)
	m.Flags = make([]bool, n)
	for i := 0; i < n; i++ {
		m.Nums[i] = int32(i - 3)
		m.Tags[i] = "tag"
		m.Inners[i] = pbwire.Inner3{Name: "grandkid", Value: int32(i + 1)}
		m.Flags[i] = i%2 == 0
	}
	return m
}

func fillOfficial(n int) *official.Scalars3 {
	m := &official.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64,
		U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64,
		Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64,
		F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: official.Color3_COLOR3_GREEN,
		Inner:  &official.Inner3{Name: "kid", Value: 42},
		OptI32: ptr(int32(-7)), OptStr: ptr("opt"),
	}
	m.Nums = make([]int32, n)
	m.Tags = make([]string, n)
	m.Inners = make([]*official.Inner3, n)
	m.Flags = make([]bool, n)
	for i := 0; i < n; i++ {
		m.Nums[i] = int32(i - 3)
		m.Tags[i] = "tag"
		m.Inners[i] = &official.Inner3{Name: "grandkid", Value: int32(i + 1)}
		m.Flags[i] = i%2 == 0
	}
	return m
}

func fillDynamic(typ *dynamic.MessageType, n int) *dynamic.Message {
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

func mbps(wire int, ns float64) float64 { return float64(wire) / ns * 1000 }

func main() {
	tmp, err := os.MkdirTemp("", "unified-desc")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(tmp)
	descPath := filepath.Join(tmp, "matrix3.desc")
	cmd := exec.Command("protoc", "-I../../c/tests/data",
		"--descriptor_set_out="+descPath, "--include_imports", "matrix3.proto")
	if out, err := cmd.CombinedOutput(); err != nil {
		fatalf("protoc: %v: %s", err, out)
	}
	desc, err := os.ReadFile(descPath)
	if err != nil {
		panic(err)
	}
	reg, err := dynamic.NewRegistry(desc)
	if err != nil {
		panic(err)
	}
	defer reg.Purge()
	dtyp, err := reg.Current().Message("structtest.Scalars3")
	if err != nil {
		panic(err)
	}
	defer dtyp.Close()

	fmt.Printf("%-7s %-6s %10s %10s %10s %10s %10s %10s\n",
		"tier", "op", "dynamic", "static", "official", "vtproto", "dyn/stat", "dyn/off")
	for _, tier := range tiers {
		goMsg := fillPB(tier.repeats)
		officialMsg := fillOfficial(tier.repeats)
		dynMsg := fillDynamic(dtyp, tier.repeats)
		want, err := proto.Marshal(officialMsg)
		if err != nil {
			panic(err)
		}
		for name, b := range map[string][]byte{
			"static":  mustBytes(goMsg.Marshal()),
			"vtproto": mustBytes(officialMsg.MarshalVT()),
			"dynamic": mustBytes(dynMsg.Encode()),
		} {
			if !bytes.Equal(b, want) {
				fatalf("%s bytes differ at %s", name, tier.label)
			}
		}
		wire := len(want)

		eDyn := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				out, err := dynMsg.Encode()
				if err != nil {
					b.Fatal(err)
				}
				_ = out
			}
		})
		dDyn := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				back, err := dtyp.Decode(want)
				if err != nil {
					b.Fatal(err)
				}
				back.Close()
			}
		})
		eStat := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				if _, err := goMsg.Marshal(); err != nil {
					b.Fatal(err)
				}
			}
		})
		dStat := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				var out pbwire.Scalars3
				if err := out.Unmarshal(want); err != nil {
					b.Fatal(err)
				}
			}
		})
		eOff := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				if _, err := proto.Marshal(officialMsg); err != nil {
					b.Fatal(err)
				}
			}
		})
		dOff := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				var out official.Scalars3
				if err := proto.Unmarshal(want, &out); err != nil {
					b.Fatal(err)
				}
			}
		})
		eVT := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				if _, err := officialMsg.MarshalVT(); err != nil {
					b.Fatal(err)
				}
			}
		})
		dVT := testing.Benchmark(func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				var out official.Scalars3
				if err := out.UnmarshalVT(want); err != nil {
					b.Fatal(err)
				}
			}
		})
		row := func(op string, dyn, stat, off, vt testing.BenchmarkResult) {
			d, s, o, v := mbps(wire, float64(dyn.NsPerOp())), mbps(wire, float64(stat.NsPerOp())),
				mbps(wire, float64(off.NsPerOp())), mbps(wire, float64(vt.NsPerOp()))
			fmt.Printf("%-7s %-6s %10.1f %10.1f %10.1f %10.1f %9.2fx %9.2fx  (%d B)\n",
				tier.label, op, d, s, o, v, d/s, d/o, wire)
		}
		row("encode", eDyn, eStat, eOff, eVT)
		row("decode", dDyn, dStat, dOff, dVT)
		dynMsg.Close()
	}
	fmt.Println("note: dynamic decode includes tree Close; others reuse/free nothing. best-of-N JIT not applied here, single testing.Benchmark each.")
}

func mustBytes(b []byte, err error) []byte {
	if err != nil {
		panic(err)
	}
	return b
}
