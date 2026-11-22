package runtime_test

// TDD gate for the shared-hint work (1.1x bar, serial small decode): the
// arena hint lives on the shared Table across all payload sizes. After a
// bulk decode records kilobytes, the FIRST small decode on the same table
// Grow(hint) for a hundred-byte payload (tens of kilobytes zeroed for one
// transition; a gateway alternating sizes pays it on every switch), and it
// also pays two atomic loads on a path that needs no shared state.
// Small inputs must neither read nor write the hint.
import (
	"runtime"
	"testing"

	pbwire "pbwire/runtime"
)

type hintProbe struct{ P []byte }

func hintProbeTable() *pbwire.Table {
	return pbwire.NewTable("hintprobe", []*pbwire.Field{{
		Num: 1, Wire: pbwire.WireLen, Kind: pbwire.KindBytes,
		GetBytes: func(m any) []byte { return m.(*hintProbe).P },
		SetBytes: func(m any, p []byte) { m.(*hintProbe).P = p },
	}})
}

func mustMarshalHint(t *testing.T, m *hintProbe) []byte {
	t.Helper()
	e := &pbwire.Encoder{}
	// Hand-rolled one-field encode: tag + length + payload, no table.
	e.Tag(1, pbwire.WireLen)
	e.Data(m.P)
	return e.Result()
}

// TestSmallDecodeIgnoresBulkArenaHint primes the table with a 50KB payload
// decode, then measures the FIRST small decode after the switch: with the
// leak it carries ~50KB, without it ~100B. The 8KB bound keeps 6x margin
// over background allocator noise for a single measured op.
func TestSmallDecodeIgnoresBulkArenaHint(t *testing.T) {
	tab := hintProbeTable()
	bigWire := mustMarshalHint(t, &hintProbe{P: make([]byte, 50000)})
	smallWire := mustMarshalHint(t, &hintProbe{P: []byte("0123456789")})
	// Warm the path so the measurement sees no first-call artifacts.
	if err := pbwire.DecodeModel(smallWire, &hintProbe{}, tab); err != nil {
		t.Fatal(err)
	}
	if err := pbwire.DecodeModel(bigWire, &hintProbe{}, tab); err != nil {
		t.Fatal(err)
	}
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	if err := pbwire.DecodeModel(smallWire, &hintProbe{}, tab); err != nil {
		t.Fatal(err)
	}
	runtime.ReadMemStats(&after)
	perOp := float64(after.TotalAlloc - before.TotalAlloc)
	t.Logf("first-small-after-bulk B/op=%.0f", perOp)
	if perOp > 8192 {
		t.Fatalf("first small decode B/op=%.0f after bulk prime, want <=8192 (hint leak)", perOp)
	}
}
