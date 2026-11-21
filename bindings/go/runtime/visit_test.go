package runtime_test

import (
	"errors"
	"math"
	"testing"

	"pbwire/runtime"
)

// visitWire builds the full-scan fixture: known Scalars3 fields in wire
// order, then an unknown varint and an unknown group, like the Java
// TestVisit.fullScan oracle.
func visitWire(t *testing.T) (wire, grp []byte, strOff, packOff, innerOff, grpOff int) {
	t.Helper()
	var b []byte
	b = runtime.AppendTag(b, 1, runtime.WireVarint)
	b = runtime.AppendVarint(b, rawI32(-42))
	b = runtime.AppendTag(b, 13, runtime.WireVarint)
	b = runtime.AppendVarint(b, 1)
	b = runtime.AppendTag(b, 11, runtime.WireFixed32)
	b = runtime.AppendFixed32(b, math.Float32bits(1.5))
	b = runtime.AppendTag(b, 12, runtime.WireFixed64)
	b = runtime.AppendFixed64(b, math.Float64bits(2.25))
	b = runtime.AppendTag(b, 14, runtime.WireLen)
	b = runtime.AppendVarint(b, uint64(len("h\xc3\xa9llo")))
	strOff = len(b)
	b = append(b, "h\xc3\xa9llo"...)
	b = runtime.AppendTag(b, 17, runtime.WireLen)
	b = runtime.AppendVarint(b, 3)
	packOff = len(b)
	b = append(b, 1, 2, 3)
	inner := runtime.AppendTag(nil, 1, runtime.WireLen)
	inner = runtime.AppendLen(inner, []byte("in"))
	b = runtime.AppendTag(b, 16, runtime.WireLen)
	b = runtime.AppendVarint(b, uint64(len(inner)))
	innerOff = len(b)
	b = append(b, inner...)
	b = runtime.AppendTag(b, 111, runtime.WireVarint)
	b = runtime.AppendVarint(b, 1)
	grpOff = len(b)
	grp = []byte{0xf3, 0x01, 0x08, 0x05, 0xf4, 0x01} // field 30, above Scalars3 range
	b = append(b, grp...)
	return b, grp, strOff, packOff, innerOff, grpOff
}

// rawI32 sign-extends like the wire: a negative int32 arrives as 64 bits.
func rawI32(v int32) uint64 { return uint64(int64(v)) }

type visitCall struct {
	field *runtime.Field
	num   uint32
	wt    runtime.WireType
	v     runtime.VisitValue
}

func TestVisitFullScan(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	wire, grp, strOff, packOff, innerOff, grpOff := visitWire(t)
	var calls []visitCall
	at, err := dt.Visit(wire, runtime.Limits{}, func(f *runtime.Field, num uint32, wt runtime.WireType, v runtime.VisitValue) error {
		calls = append(calls, visitCall{f, num, wt, v})
		return nil
	})
	if err != nil {
		t.Fatalf("Visit: %v", err)
	}
	if at != len(wire) {
		t.Fatalf("failOff = %d, want wire length %d", at, len(wire))
	}
	wantNums := []uint32{1, 13, 11, 12, 14, 17, 16, 111, 30}
	if len(calls) != len(wantNums) {
		t.Fatalf("got %d callbacks, want %d", len(calls), len(wantNums))
	}
	for i, n := range wantNums {
		if calls[i].num != n {
			t.Fatalf("call %d: num = %d, want %d", i, calls[i].num, n)
		}
	}
	// Raw wire bits: varints unfolded, no zigzag or sign extension applied.
	if calls[0].v.U64 != rawI32(-42) {
		t.Errorf("int32 raw bits = %x, want sign-extended -42", calls[0].v.U64)
	}
	if calls[1].v.U64 != 1 {
		t.Errorf("bool raw bit = %d, want 1", calls[1].v.U64)
	}
	if calls[2].v.U64 != uint64(math.Float32bits(1.5)) {
		t.Errorf("float raw bits = %x", calls[2].v.U64)
	}
	if calls[3].v.U64 != math.Float64bits(2.25) {
		t.Errorf("double raw bits = %x", calls[3].v.U64)
	}
	// Known fields resolve; the packed run reports wire type LEN.
	for i, name := range []string{"i32", "flag", "f32", "f64", "str", "nums", "inner"} {
		if calls[i].field == nil || calls[i].field.Name != name {
			t.Errorf("call %d: field = %+v, want name %q", i, calls[i].field, name)
		}
	}
	if calls[5].wt != runtime.WireLen {
		t.Errorf("packed numbers wt = %v, want LEN", calls[5].wt)
	}
	for i, wt := range []runtime.WireType{
		runtime.WireVarint, runtime.WireVarint, runtime.WireFixed32,
		runtime.WireFixed64, runtime.WireLen, runtime.WireLen, runtime.WireLen,
		runtime.WireVarint, runtime.WireStartGroup,
	} {
		if calls[i].wt != wt {
			t.Errorf("call %d: wt = %v, want %v", i, calls[i].wt, wt)
		}
	}
	// LEN payloads alias the input; the group arrives as the raw frame.
	for i, off := range map[int]int{4: strOff, 5: packOff, 6: innerOff, 8: grpOff} {
		p := calls[i].v.Data
		if len(p) == 0 {
			t.Errorf("call %d: empty payload", i)
			continue
		}
		if len(wire) <= off || &p[0] != &wire[off] {
			t.Errorf("call %d: payload does not alias the input", i)
		}
	}
	if got := string(calls[4].v.Data); got != "h\xc3\xa9llo" {
		t.Errorf("string payload = %q", got)
	}
	if len(calls[5].v.Data) != 3 || calls[5].v.Data[0] != 1 || calls[5].v.Data[2] != 3 {
		t.Errorf("packed payload = %v, want [1 2 3]", calls[5].v.Data)
	}
	innerType, err := s.Type("structtest.Inner3")
	if err != nil {
		t.Fatal(err)
	}
	inner, err := innerType.Decode(calls[6].v.Data)
	if err != nil {
		t.Fatalf("decode LEN payload: %v", err)
	}
	name, ok, err := inner.GetString("name")
	if err != nil || !ok || name != "in" {
		t.Errorf("nested payload name = %q,%v,%v", name, ok, err)
	}
	// Unknown numbers resolve to a nil field.
	if calls[7].field != nil {
		t.Errorf("unknown field 111: field = %+v, want nil", calls[7].field)
	}
	if calls[7].v.U64 != 1 {
		t.Errorf("unknown varint raw = %d, want 1", calls[7].v.U64)
	}
	if calls[8].field != nil {
		t.Errorf("unknown group: field = %+v, want nil", calls[8].field)
	}
	if string(calls[8].v.Data) != string(grp) {
		t.Errorf("group frame = %x, want %x", calls[8].v.Data, grp)
	}
}

func TestVisitAbort(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	var b []byte
	b = runtime.AppendTag(b, 1, runtime.WireVarint)
	b = runtime.AppendVarint(b, rawI32(-42))
	b = runtime.AppendTag(b, 2, runtime.WireVarint)
	b = runtime.AppendVarint(b, 1<<40)
	// Field 1 is tag 0x08 plus a 10-byte varint, so field 2 starts at 11.
	boom := errors.New("boom")
	calls := 0
	at, err := dt.Visit(b, runtime.Limits{}, func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		calls++
		if calls == 2 {
			return boom
		}
		return nil
	})
	if !errors.Is(err, boom) {
		t.Fatalf("err = %v, want boom", err)
	}
	if calls != 2 {
		t.Fatalf("calls = %d, want 2", calls)
	}
	if at != 11 {
		t.Fatalf("failOff = %d, want 11", at)
	}
	if b[at] != 0x10 {
		t.Fatalf("wire[failOff] = %#x, want second field tag 0x10", b[at])
	}
}

func TestVisitLimits(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	var b []byte
	b = runtime.AppendTag(b, 1, runtime.WireVarint)
	b = runtime.AppendVarint(b, 1)
	b = runtime.AppendTag(b, 14, runtime.WireLen)
	b = runtime.AppendLen(b, []byte("way too long"))
	if len(b) <= 10 {
		t.Fatalf("fixture is %d bytes, want over the 10-byte cap", len(b))
	}
	at, err := dt.Visit(b, runtime.Limits{MaxBytes: 10}, func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		return nil
	})
	if !errors.Is(err, runtime.ErrLimit) {
		t.Fatalf("err = %v, want ErrLimit", err)
	}
	if at != 0 {
		t.Fatalf("failOff = %d, want 0", at)
	}
}

func TestVisitMalformed(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	var full []byte
	full = runtime.AppendTag(full, 1, runtime.WireVarint)
	full = runtime.AppendVarint(full, rawI32(-42))
	var strWire []byte
	strWire = runtime.AppendTag(strWire, 14, runtime.WireLen)
	strWire = runtime.AppendLen(strWire, []byte("hello"))
	nop := func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		return nil
	}
	overlong := []byte{0x08}
	for i := 0; i < 10; i++ {
		overlong = append(overlong, 0x80)
	}
	overlong = append(overlong, 0x01)
	cases := []struct {
		name string
		wire []byte
		at   int
	}{
		{"truncated varint", full[:len(full)-1], 0},
		{"field 0", []byte{0x00}, 0},
		{"bad wire type", []byte{0x0f}, 0},
		{"two-byte field 0", []byte{0x80, 0x00}, 0},
		{"top-level end group", []byte{0x0c}, 0},
		{"truncated fixed32", []byte{0xd5, 0x05, 0x01, 0x02}, 0},
		{"truncated fixed64", []byte{0xd1, 0x05, 0x01, 0x02, 0x03}, 0},
		{"cut length prefix", []byte{0x42, 0x80}, 0},
		{"missing length", []byte{0x42}, 0},
		{"overlong varint", overlong, 0},
		{"mismatched group end", []byte{0xa3, 0x08, 0x05, 0xac}, 0},
		{"unclosed group", []byte{0xa3, 0x08, 0x05}, 0},
		{"truncated LEN", strWire[:len(strWire)-2], 0},
		{"bad field after good", []byte{0x08, 0x01, 0xd5, 0x05, 0x01, 0x02}, 2},
	}
	for _, tc := range cases {
		at, err := dt.Visit(tc.wire, runtime.Limits{}, nop)
		if err == nil {
			t.Errorf("%s: expected error", tc.name)
			continue
		}
		if at != tc.at {
			t.Errorf("%s: failOff = %d, want %d", tc.name, at, tc.at)
		}
	}
}

func TestVisitDeepGroups(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	nop := func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		return nil
	}
	deep := func(n int) []byte {
		b := make([]byte, 2*n)
		for i := 0; i < n; i++ {
			b[i] = 0x0b // field 1 start group
		}
		for i := n; i < 2*n; i++ {
			b[i] = 0x0c // field 1 end group
		}
		return b
	}
	calls := 0
	at, err := dt.Visit(deep(300), runtime.Limits{}, func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		calls++
		return nil
	})
	if !errors.Is(err, runtime.ErrDepth) {
		t.Fatalf("300-deep: err = %v, want ErrDepth", err)
	}
	if at != 0 || calls != 0 {
		t.Fatalf("300-deep: failOff = %d calls = %d, want 0 and 0", at, calls)
	}
	// The cap is 256: exactly 256 nested frames still scan, as one raw frame.
	at, err = dt.Visit(deep(256), runtime.Limits{}, nop)
	if err != nil {
		t.Fatalf("256-deep: %v", err)
	}
	if at != 512 {
		t.Fatalf("256-deep: failOff = %d, want 512", at)
	}
}

func TestVisitEmpty(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	calls := 0
	at, err := dt.Visit(nil, runtime.Limits{}, func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		calls++
		return nil
	})
	if err != nil {
		t.Fatalf("Visit(nil): %v", err)
	}
	if at != 0 || calls != 0 {
		t.Fatalf("Visit(nil): failOff = %d calls = %d, want 0 and 0", at, calls)
	}
}

func TestVisitNilArgs(t *testing.T) {
	s := mustLoad(t, "testdata/matrix3.desc")
	dt, err := s.Type("structtest.Scalars3")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := dt.Visit([]byte{0x08, 0x01}, runtime.Limits{}, nil); err == nil {
		t.Error("nil visitor should fail")
	}
	var nilType *runtime.DType
	if _, err := nilType.Visit([]byte{0x08, 0x01}, runtime.Limits{}, func(*runtime.Field, uint32, runtime.WireType, runtime.VisitValue) error {
		return nil
	}); err == nil {
		t.Error("nil type should fail")
	}
}
