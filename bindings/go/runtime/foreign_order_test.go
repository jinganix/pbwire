// Foreign rows keep first-seen order under deterministic encode: native
// map-entry rows sort by key, but a sibling's bytes ride verbatim in
// table order. Two foreign rows in non-numeric table order pin it.
package runtime_test

import (
	"testing"

	"pbwire/runtime"
	dep "pbwire/tests/gen/xdep"
)

func TestForeignDeterministicKeepsFirstSeen(t *testing.T) {
	mk := func(num uint32, sel func(*foreignPair) *dep.DepMsg) *runtime.Field {
		return &runtime.Field{
			Num: num, Wire: runtime.WireLen, Kind: runtime.KindMessage,
			Foreign: &runtime.ForeignOps{
				Encode: func(m any) ([]byte, error) { return m.(*dep.DepMsg).Marshal() },
				Decode: func(_ any, w []byte) error { return (&dep.DepMsg{}).Unmarshal(w) },
			},
			GetMsg: func(m any) any { return sel(m.(*foreignPair)) },
		}
	}
	h := &foreignPair{hi: &dep.DepMsg{Name: "hi", Value: 1}, lo: &dep.DepMsg{Name: "lo", Value: 2}}
	tab := runtime.NewTable("test.ForeignOrder", []*runtime.Field{
		mk(20, func(p *foreignPair) *dep.DepMsg { return p.hi }),
		mk(10, func(p *foreignPair) *dep.DepMsg { return p.lo }),
	})
	wire, err := runtime.EncodeModelDeterministic(h, tab)
	if err != nil {
		t.Fatal(err)
	}
	var order []uint32
	d := runtime.Decoder{Buf: wire}
	for !d.Done() {
		num, wt, err := d.Next()
		if err != nil {
			t.Fatal(err)
		}
		order = append(order, num)
		if err := d.Skip(num, wt, 0); err != nil {
			t.Fatal(err)
		}
	}
	if len(order) != 2 || order[0] != 20 || order[1] != 10 {
		t.Fatalf("foreign order = %v, want [20 10] (table order, not sorted)", order)
	}
}

type foreignPair struct {
	hi *dep.DepMsg
	lo *dep.DepMsg
}
