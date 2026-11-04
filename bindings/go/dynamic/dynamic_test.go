package dynamic

import (
	"bytes"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

var testDesc []byte

func TestMain(m *testing.M) {
	// Build the descriptor set once with protoc, like the Java bench does.
	// Needs protoc (>= 3.x); the repo's go-bench needs it too.
	tmp, err := os.MkdirTemp("", "dyn-desc")
	if err != nil {
		panic(err)
	}
	defer os.RemoveAll(tmp)
	out := filepath.Join(tmp, "matrix3.desc")
	cmd := exec.Command("protoc", "-I../../c/tests/data",
		"--descriptor_set_out="+out, "--include_imports", "matrix3.proto")
	if out2, err := cmd.CombinedOutput(); err != nil {
		panic("protoc: " + err.Error() + ": " + string(out2))
	}
	desc, err := os.ReadFile(out)
	if err != nil {
		panic(err)
	}
	testDesc = desc
	os.Exit(m.Run())
}

func fillScalars(t *testing.T, typ *MessageType, n int) *Message {
	t.Helper()
	m := typ.New()
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	must(m.SetInt("i32", math.MinInt32))
	must(m.SetInt("i64", math.MaxInt64))
	must(m.SetUint("u32", 0xffffffff))
	must(m.SetUint("u64", 1<<64-1))
	must(m.SetInt("s32", math.MinInt32))
	must(m.SetInt("s64", math.MinInt64))
	must(m.SetUint("fx32", 0xdeadbeef))
	must(m.SetUint("fx64", 72623859790382856))
	must(m.SetInt("sfx32", math.MinInt32))
	must(m.SetInt("sfx64", math.MinInt64))
	must(m.SetFloat("f32", float32(math.Inf(1))))
	must(m.SetDouble("f64", -123456.789))
	must(m.SetBool("flag", true))
	must(m.SetString("str", "hello"))
	must(m.SetInt("color", 2))
	must(m.SetInt("opt_i32", -7))
	must(m.SetString("opt_str", "opt"))
	inner, err := m.NewMessage("inner")
	must(err)
	must(inner.SetString("name", "kid"))
	must(inner.SetInt("value", 42))
	for i := 0; i < n; i++ {
		must(m.SetInt("nums", int64(i-3)))
		must(m.SetString("tags", "tag"))
		must(m.SetBool("flags", i%2 == 0))
		sub, err := m.NewMessage("inners")
		must(err)
		must(sub.SetString("name", "grandkid"))
		must(sub.SetInt("value", int64(i+1)))
	}
	return m
}

var tiers = []struct {
	repeats int
	want    int
}{{0, 136}, {1, 174}, {100, 2469}, {3000, 74913}}

func TestTiersParity(t *testing.T) {
	r, err := NewRegistry(testDesc)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Purge()
	typ, err := r.Current().Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	defer typ.Close()
	for _, tier := range tiers {
		m := fillScalars(t, typ, tier.repeats)
		wire, err := m.Encode()
		if err != nil {
			t.Fatal(err)
		}
		if len(wire) != tier.want {
			t.Fatalf("x%d: got %d bytes, want %d", tier.repeats, len(wire), tier.want)
		}
		back, err := typ.Decode(wire)
		if err != nil {
			t.Fatal(err)
		}
		again, err := back.Encode()
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(wire, again) {
			t.Fatalf("x%d: round trip mismatch", tier.repeats)
		}
		back.Close()
		m.Close()
	}
}

func TestHotSwap(t *testing.T) {
	r, err := NewRegistry(testDesc)
	if err != nil {
		t.Fatal(err)
	}
	v1 := r.Current()
	t1, err := v1.Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	defer t1.Close()
	old := fillScalars(t, t1, 0)
	defer old.Close()
	before, err := old.Encode()
	if err != nil {
		t.Fatal(err)
	}

	// Swap to a new version (same bytes here; any descriptor set takes the
	// same path). The failed-load path must also leave v1 live: try garbage.
	if _, err := r.Swap([]byte("not a descriptor set")); err == nil {
		t.Fatal("Swap of garbage unexpectedly succeeded")
	}
	if r.Current() != v1 {
		t.Fatal("failed Swap changed the live version")
	}
	v2, err := r.Swap(testDesc)
	if err != nil {
		t.Fatal(err)
	}
	if v2.version != 2 {
		t.Fatalf("version = %d, want 2", v2.version)
	}
	// In-flight v1 message still encodes after the swap and after Purge.
	r.Purge()
	after, err := old.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("v1 message changed across swap")
	}
	t2, err := r.Current().Message("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	defer t2.Close()
	fresh := fillScalars(t, t2, 0)
	defer fresh.Close()
	fwire, err := fresh.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, fwire) {
		t.Fatal("v2 encodes differently from v1")
	}
}
