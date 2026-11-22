package runtime_test

// TDD gate for the decode-dispatch work (1.1x bar): resolving a repeated
// message row's Sub table once per occurrence is pure overhead — generated
// tables return a static table, dynamic ones memoize under their own lock,
// so the func is pure per field. A 100-element repeated message pays 100
// indirect calls; memoizing pays one.
import (
	"bytes"
	"sync/atomic"
	"testing"

	"pbwire/runtime"
)

type subMemoKid struct{ V int32 }
type subMemoParent struct{ Kids []*subMemoKid }

func subMemoTables(calls *atomic.Int32) *runtime.Table {
	kid := runtime.NewTable("kid", []*runtime.Field{{
		Num: 1, Wire: runtime.WireVarint, Kind: runtime.KindInt32,
		GetI32: func(m any) int32 { return m.(*subMemoKid).V },
		SetI32: func(m any, v int32) { m.(*subMemoKid).V = v },
	}})
	row := &runtime.Field{
		Num: 1, Wire: runtime.WireLen, Kind: runtime.KindMessage, Repeated: true,
		Sub: func() *runtime.Table { calls.Add(1); return kid },
		NewRepMsg: func(m any) any {
			k := &subMemoKid{}
			p := m.(*subMemoParent)
			p.Kids = append(p.Kids, k)
			return k
		},
	}
	return runtime.NewTable("parent", []*runtime.Field{row})
}

// TestSubTableMemoized pins the resolution budget: three occurrences of a
// repeated message field resolve Sub exactly once.
func TestSubTableMemoized(t *testing.T) {
	var calls atomic.Int32
	parent := subMemoTables(&calls)
	wire := bytes.Repeat([]byte{0x0A, 0x02, 0x08, 0x01}, 3)
	d := &runtime.Decoder{Buf: wire}
	a := &runtime.Arena{}
	m := &subMemoParent{}
	if err := runtime.DecodeTable(d, a, m, parent, 0); err != nil {
		t.Fatal(err)
	}
	if len(m.Kids) != 3 {
		t.Fatalf("decoded %d kids, want 3", len(m.Kids))
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("Sub() resolved %d times for 3 occurrences, want 1 (memoized)", got)
	}
}
