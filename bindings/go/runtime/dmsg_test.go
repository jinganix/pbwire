// Dynamic Go core: the same wire as the struct codec, decoded into a value
// tree built from a runtime schema, and unknown fields retained per message.
package runtime_test

import (
	"bytes"
	"math"
	"os"
	"testing"

	"pbwire/runtime"
	"pbwire/tests/gen/matrix3"
)

func fillScalarsStruct(n int) *matrix3.Scalars3 {
	i32v, strv := int32(-7), "opt"
	m := &matrix3.Scalars3{
		I32: math.MinInt32, I64: math.MaxInt64, U32: math.MaxUint32, U64: math.MaxUint64,
		S32: math.MinInt32, S64: math.MinInt64, Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: math.MinInt32, Sfx64: math.MinInt64, F32: float32(math.Inf(1)), F64: -123456.789,
		Flag: true, Str: "hello", Color: matrix3.Color3_COLOR3_GREEN,
		Inner:  &matrix3.Inner3{Name: "kid", Value: 42},
		OptI32: &i32v, OptStr: &strv,
	}
	for i := 0; i < n; i++ {
		m.Nums = append(m.Nums, int32(i-3))
		m.Tags = append(m.Tags, "tag")
		m.Inners = append(m.Inners, &matrix3.Inner3{Name: "grandkid", Value: int32(i + 1)})
		m.Flags = append(m.Flags, i%2 == 0)
	}
	return m
}

func loadMatrix3(t *testing.T) ([]byte, *runtime.Schema) {
	t.Helper()
	raw, err := os.ReadFile("testdata/matrix3.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	return raw, s
}

var dmsgTiers = []struct{ n, want int }{{0, 136}, {1, 174}, {100, 2469}, {3000, 74913}}

func TestDMsgTiersParity(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	for _, tier := range dmsgTiers {
		src := fillScalarsStruct(tier.n)
		ref, err := src.Marshal()
		if err != nil {
			t.Fatal(err)
		}
		if len(ref) != tier.want {
			t.Fatalf("tier %d: struct encoder wrote %d bytes, want %d", tier.n, len(ref), tier.want)
		}
		back, err := typ.Decode(ref)
		if err != nil {
			t.Fatal(err)
		}
		again, err := back.Encode()
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(again, ref) {
			t.Fatalf("tier %d: %d vs %d bytes", tier.n, len(again), len(ref))
		}
		if tier.n > 0 {
			nums, err := back.GetIntSlice("nums")
			if err != nil || len(nums) != tier.n || nums[tier.n-1] != int64(tier.n-4) {
				t.Fatalf("tier %d: nums = %v (err %v)", tier.n, nums, err)
			}
			tags, err := back.GetStringSlice("tags")
			if err != nil || len(tags) != tier.n || tags[0] != "tag" {
				t.Fatalf("tier %d: tags = %v (err %v)", tier.n, tags, err)
			}
		}
		f64, ok, err := back.GetDouble("f64")
		if err != nil || !ok || f64 != -123456.789 {
			t.Fatalf("tier %d: f64 = %v ok=%v err=%v", tier.n, f64, ok, err)
		}
		s32, _, err := back.GetInt("s32")
		if err != nil || s32 != math.MinInt32 {
			t.Fatalf("tier %d: s32 = %d (%v)", tier.n, s32, err)
		}
		if got, ok, err := back.GetString("str"); err != nil || !ok || got != "hello" {
			t.Fatalf("tier %d: str = %q ok=%v err=%v", tier.n, got, ok, err)
		}
		inner, ok, err := back.GetMessage("inner")
		if err != nil || !ok || inner == nil {
			t.Fatalf("tier %d: inner missing (%v)", tier.n, err)
		}
		if v, _, err := inner.GetInt("value"); err != nil || v != 42 {
			t.Fatalf("tier %d: inner.value = %d (%v)", tier.n, v, err)
		}
	}
}

// A tree built through the setters encodes to the same bytes as the struct
// model filled with the same values: the two front-ends share one wire.
func TestDMsgBuildMatchesStruct(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	ref, err := fillScalarsStruct(1).Marshal()
	if err != nil {
		t.Fatal(err)
	}
	m := typ.New()
	set := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	// Values are semantic: a negative sint32 goes in as -7 and comes back
	// as -7, the engine does the zigzag on the wire.
	set(m.SetInt("i32", math.MinInt32))
	set(m.SetInt("i64", math.MaxInt64))
	set(m.SetUint("u32", math.MaxUint32))
	set(m.SetUint("u64", math.MaxUint64))
	set(m.SetInt("s32", math.MinInt32))
	set(m.SetInt("s64", math.MinInt64))
	set(m.SetUint("fx32", 0xdeadbeef))
	set(m.SetUint("fx64", 72623859790382856))
	set(m.SetInt("sfx32", math.MinInt32))
	set(m.SetInt("sfx64", math.MinInt64))
	set(m.SetFloat("f32", float32(math.Inf(1))))
	set(m.SetDouble("f64", -123456.789))
	set(m.SetBool("flag", true))
	set(m.SetString("str", "hello"))
	set(m.SetInt("color", 2))
	set(m.SetInt("opt_i32", -7))
	set(m.SetString("opt_str", "opt"))
	inner, err := m.NewMessage("inner")
	set(err)
	set(inner.SetString("name", "kid"))
	set(inner.SetInt("value", 42))
	for i := 0; i < 1; i++ {
		set(m.SetInt("nums", int64(i-3)))
		set(m.SetString("tags", "tag"))
		set(m.SetBool("flags", i%2 == 0))
		sub, err := m.NewMessage("inners")
		set(err)
		set(sub.SetString("name", "grandkid"))
		set(sub.SetInt("value", int64(i+1)))
	}
	got, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, ref) {
		t.Fatalf("tree build % x\nstruct    % x", got, ref)
	}
}

func TestDMsgUnknownNested(t *testing.T) {
	_, s := loadMatrix3(t)
	typ, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	// inner is field 16: name="k" plus an undeclared field 8 as varint 7.
	wire := []byte{0x82, 0x01, 0x05, 0x0a, 0x01, 0x6b, 0x40, 0x07}
	m, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	inner, ok, err := m.GetMessage("inner")
	if err != nil || !ok || inner == nil {
		t.Fatalf("inner: ok=%v err=%v", ok, err)
	}
	if got := inner.UnknownBytes(); len(got) != 2 || got[0] != 0x40 || got[1] != 0x07 {
		t.Fatalf("unknown = % x, want 40 07", got)
	}
	out, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(out, wire) {
		t.Fatalf("re-encode % x, want % x", out, wire)
	}
	if got := m.UnknownBytes(); len(got) != 0 {
		t.Fatalf("root retained nested bytes: % x", got)
	}
}

// Oneof and map structures go through the same runtime tables: the last oneof
// member wins, and map entries ride as repeated entry messages.
func TestDMsgOneofAndMap(t *testing.T) {
	raw, err := os.ReadFile("testdata/oneof1.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err := runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	typ, err := s.Type("spectest.Oneof1")
	if err != nil {
		t.Fatal(err)
	}
	m := typ.New()
	if err := m.SetString("name", "n"); err != nil {
		t.Fatal(err)
	}
	if err := m.SetInt("num", 5); err != nil {
		t.Fatal(err)
	}
	if err := m.SetString("text", "t"); err != nil {
		t.Fatal(err)
	}
	wire, err := m.Encode()
	if err != nil {
		t.Fatal(err)
	}
	back, err := typ.Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok, err := back.GetInt("num"); err != nil || ok {
		t.Fatalf("num still set after text won: ok=%v err=%v", ok, err)
	}
	if got, ok, err := back.GetString("text"); err != nil || !ok || got != "t" {
		t.Fatalf("text = %q ok=%v err=%v", got, ok, err)
	}

	raw, err = os.ReadFile("testdata/maptest.desc")
	if err != nil {
		t.Fatal(err)
	}
	s, err = runtime.Load(raw)
	if err != nil {
		t.Fatal(err)
	}
	maps, err := s.Type("spectest.MapTest")
	if err != nil {
		t.Fatal(err)
	}
	mm := maps.New()
	e1, err := mm.NewMessage("by_int")
	if err != nil {
		t.Fatal(err)
	}
	if err := e1.SetInt("key", 7); err != nil {
		t.Fatal(err)
	}
	if err := e1.SetString("value", "seven"); err != nil {
		t.Fatal(err)
	}
	e2, err := mm.NewMessage("by_msg")
	if err != nil {
		t.Fatal(err)
	}
	if err := e2.SetString("key", "k"); err != nil {
		t.Fatal(err)
	}
	val, err := e2.NewMessage("value")
	if err != nil {
		t.Fatal(err)
	}
	if err := val.SetString("name", "v"); err != nil {
		t.Fatal(err)
	}
	if err := val.SetInt("value", 9); err != nil {
		t.Fatal(err)
	}
	mwire, err := mm.Encode()
	if err != nil {
		t.Fatal(err)
	}
	mback, err := maps.Decode(mwire)
	if err != nil {
		t.Fatal(err)
	}
	entries, err := mback.GetMessageSlice("by_int")
	if err != nil || len(entries) != 1 {
		t.Fatalf("by_int entries = %d (err %v)", len(entries), err)
	}
	msgEntries, err := mback.GetMessageSlice("by_msg")
	if err != nil || len(msgEntries) != 1 {
		t.Fatalf("by_msg entries = %d (err %v)", len(msgEntries), err)
	}
}
