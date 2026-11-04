//go:build bench

// Go binding benchmark against the official protobuf-go generated code on
// the same matrix3.proto and the four standard tiers from BENCHMARKS.md.
//
// The same payload as every other binding: every scalar kind, the nested
// message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
// x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes.
// Both sides must produce identical bytes before anything is timed, and both
// allocate per call (pbwire Encode()/Decode() against proto.Marshal()/
// proto.Unmarshal()), so the comparison is like for like.
//
// The gen/ directory is built by bench.sh (protoc-gen-go) and never
// committed. Correctness gates run in TestMain before any timing: a speed
// number from a wrong codec is meaningless.
//
//	./bench.sh          generate, build, run
package benchmarks

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
	"pbwire/benchmarks/gen"
	"pbwire/dynamic"
)

type tier struct {
	label string
	n     int
	want  int
	floor int
}

var tiers = []tier{
	{"small", 0, 136, 50000},
	{"medium", 1, 174, 50000},
	{"large", 100, 2469, 5000},
	{"xlarge", 3000, 74913, 500},
}

const minPassSeconds = 0.25
const passes = 3

var (
	descBytes []byte
	reg       *dynamic.Registry
	typ       *dynamic.MessageType
)

func i32p(v int32) *int32   { return &v }
func strp(v string) *string { return &v }

func fillOfficial(n int) *gen.Scalars3 {
	m := &gen.Scalars3{
		I32:    math.MinInt32,
		I64:    math.MaxInt64,
		U32:    math.MaxUint32,
		U64:    math.MaxUint64,
		S32:    math.MinInt32,
		S64:    math.MinInt64,
		Fx32:   0xdeadbeef,
		Fx64:   72623859790382856,
		Sfx32:  math.MinInt32,
		Sfx64:  math.MinInt64,
		F32:    float32(math.Inf(1)),
		F64:    -123456.789,
		Flag:   true,
		Str:    "hello",
		Color:  gen.Color3_COLOR3_GREEN,
		Inner:  &gen.Inner3{Name: "kid", Value: 42},
		OptI32: i32p(-7),
		OptStr: strp("opt"),
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &gen.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func fillPbwire(n int) (*dynamic.Message, error) {
	m := typ.New()
	set := func(err error) error {
		if err != nil {
			m.Close()
		}
		return err
	}
	if err := set(m.SetInt("i32", math.MinInt32)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("i64", math.MaxInt64)); err != nil {
		return nil, err
	}
	if err := set(m.SetUint("u32", 0xffffffff)); err != nil {
		return nil, err
	}
	if err := set(m.SetUint("u64", 1<<64-1)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("s32", math.MinInt32)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("s64", math.MinInt64)); err != nil {
		return nil, err
	}
	if err := set(m.SetUint("fx32", 0xdeadbeef)); err != nil {
		return nil, err
	}
	if err := set(m.SetUint("fx64", 72623859790382856)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("sfx32", math.MinInt32)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("sfx64", math.MinInt64)); err != nil {
		return nil, err
	}
	if err := set(m.SetFloat("f32", float32(math.Inf(1)))); err != nil {
		return nil, err
	}
	if err := set(m.SetDouble("f64", -123456.789)); err != nil {
		return nil, err
	}
	if err := set(m.SetBool("flag", true)); err != nil {
		return nil, err
	}
	if err := set(m.SetString("str", "hello")); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("color", 2)); err != nil {
		return nil, err
	}
	if err := set(m.SetInt("opt_i32", -7)); err != nil {
		return nil, err
	}
	if err := set(m.SetString("opt_str", "opt")); err != nil {
		return nil, err
	}
	inner, err := m.NewMessage("inner")
	if err != nil {
		m.Close()
		return nil, err
	}
	if err := inner.SetString("name", "kid"); err != nil {
		m.Close()
		return nil, err
	}
	if err := inner.SetInt("value", 42); err != nil {
		m.Close()
		return nil, err
	}
	for i := 0; i < n; i++ {
		if err := set(m.SetInt("nums", int64(i-3))); err != nil {
			return nil, err
		}
		if err := set(m.SetString("tags", "tag")); err != nil {
			return nil, err
		}
		if err := set(m.SetBool("flags", i%2 == 0)); err != nil {
			return nil, err
		}
		sub, err := m.NewMessage("inners")
		if err != nil {
			m.Close()
			return nil, err
		}
		if err := sub.SetString("name", "grandkid"); err != nil {
			m.Close()
			return nil, err
		}
		if err := sub.SetInt("value", int64(i+1)); err != nil {
			m.Close()
			return nil, err
		}
	}
	return m, nil
}

// The gate: every tier's official bytes land on the pinned size, pbwire
// encodes byte-identical bytes, and a pbwire decode re-encodes
// byte-identically. Official-side values are spot-checked so a wrong payload
// cannot hide behind matching bytes.
func gate(t tier) error {
	reference, err := proto.Marshal(fillOfficial(t.n))
	if err != nil {
		return err
	}
	if len(reference) != t.want {
		return fmt.Errorf("%s: official encoder wrote %d bytes, expected %d", t.label, len(reference), t.want)
	}
	message, err := fillPbwire(t.n)
	if err != nil {
		return err
	}
	defer message.Close()
	got, err := message.Encode()
	if err != nil {
		return err
	}
	if !bytes.Equal(got, reference) {
		return fmt.Errorf("%s: pbwire-go and protobuf-go encode differently (%d vs %d bytes)", t.label, len(got), len(reference))
	}
	back, err := typ.Decode(reference)
	if err != nil {
		return err
	}
	defer back.Close()
	again, err := back.Encode()
	if err != nil {
		return err
	}
	if !bytes.Equal(again, reference) {
		return fmt.Errorf("%s: pbwire-go decode differs", t.label)
	}
	var official gen.Scalars3
	if err := proto.Unmarshal(reference, &official); err != nil {
		return err
	}
	if official.Str != "hello" || official.Inner.GetName() != "kid" ||
		official.Inner.GetValue() != 42 || len(official.Nums) != t.n ||
		(t.n > 0 && official.Inners[t.n-1].GetValue() != int32(t.n)) {
		return fmt.Errorf("%s: official decoded values differ", t.label)
	}
	fmt.Printf("OK   %s encoding parity, x%d (%d bytes)\n", t.label, t.n, len(reference))
	return nil
}

func TestMain(m *testing.M) {
	_, file, _, _ := runtime.Caller(0)
	var err error
	descBytes, err = os.ReadFile(filepath.Join(filepath.Dir(file), "gen", "matrix3.desc"))
	if err != nil {
		fmt.Fprintln(os.Stderr, "load gen/matrix3.desc: run ./bench.sh first")
		os.Exit(1)
	}
	reg, err = dynamic.NewRegistry(descBytes)
	if err != nil {
		fmt.Fprintln(os.Stderr, "NewRegistry:", err)
		os.Exit(1)
	}
	defer reg.Purge()
	typ, err = reg.Current().Message("structtest.Scalars3")
	if err != nil {
		fmt.Fprintln(os.Stderr, "Message:", err)
		os.Exit(1)
	}
	defer typ.Close()
	for _, t := range tiers {
		if err := gate(t); err != nil {
			fmt.Fprintln(os.Stderr, "FAIL:", err)
			os.Exit(1)
		}
	}
	os.Exit(m.Run())
}

func run(count int, op func()) float64 {
	start := time.Now()
	for i := 0; i < count; i++ {
		op()
	}
	return float64(time.Since(start).Nanoseconds()) / float64(count)
}

// Every op calibrates independently from the floor: a slow op already exceeds
// the minimum at the floor and keeps it, a fast op doubles up.
func calibrate(op func(), floor int) int {
	count := floor
	if count < 1 {
		count = 1
	}
	for {
		start := time.Now()
		for i := 0; i < count; i++ {
			op()
		}
		if time.Since(start).Seconds() >= minPassSeconds || count >= 100000000 {
			return count
		}
		count *= 2
	}
}

func measure(floor int, op func()) float64 {
	count := calibrate(op, floor)
	run(count, op) // warm-up
	best := float64(1e100)
	for p := 0; p < passes; p++ {
		if ns := run(count, op); ns < best {
			best = ns
		}
	}
	return best
}

func row(impl, op string, size int, ns float64) {
	fmt.Printf("  %-12s %-7s %12.1f %10.1f\n", impl, op, ns, float64(size)/ns*1000.0)
}

func TestMatrix3Bench(t *testing.T) {
	for _, tr := range tiers {
		message, err := fillPbwire(tr.n)
		if err != nil {
			t.Fatal(err)
		}
		reference, err := proto.Marshal(fillOfficial(tr.n))
		if err != nil {
			t.Fatal(err)
		}
		official := fillOfficial(tr.n)

		// Tier line shows the reference op's count; slower ops keep the floor.
		count := calibrate(func() {
			_, _ = message.Encode()
		}, tr.floor)
		encode := measure(tr.floor, func() {
			sinkBytes, _ = message.Encode()
		})
		decode := measure(tr.floor, func() {
			m, _ := typ.Decode(reference)
			m.Close()
		})
		officialEncode := measure(tr.floor, func() {
			sinkBytes, _ = proto.Marshal(official)
		})
		officialDecode := measure(tr.floor, func() {
			var m gen.Scalars3
			_ = proto.Unmarshal(reference, &m)
		})
		message.Close()

		fmt.Printf("\n%s: %d bytes, %d iterations (repeat field x%d)\n", tr.label, len(reference), count, tr.n)
		fmt.Printf("  %-12s %-7s %12s %10s\n", "impl", "op", "ns/op", "MB/s")
		row("pbwire-go", "encode", len(reference), encode)
		row("protobuf-go", "encode", len(reference), officialEncode)
		row("pbwire-go", "decode", len(reference), decode)
		row("protobuf-go", "decode", len(reference), officialDecode)
		fmt.Println()
	}
}

var sinkBytes []byte
