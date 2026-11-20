package tests

// Scratch CI probe (debug/ci-probe only, never merge): dump exactly how the
// matrix3.bin round-trip differs on linux/amd64.
import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	gen1 "pbwire/tests/gen/matrix3"
)

func TestZZDebugRoundTrip(t *testing.T) {
	ref, err := os.ReadFile(filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "c", "tests", "data", "matrix3.bin"))
	if err != nil {
		t.Fatal(err)
	}
	m := &gen1.Scalars3{}
	if err := m.Unmarshal(ref); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	out, err := m.Marshal()
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	fmt.Printf("ZZ ref len=%d out len=%d equal=%v\n", len(ref), len(out), bytes.Equal(out, ref))
	if !bytes.Equal(out, ref) {
		n := 0
		for i := 0; i < len(ref) && i < len(out) && n < 10; i++ {
			if ref[i] != out[i] {
				fmt.Printf("ZZ diff at %d: ref=%02x out=%02x\n", i, ref[i], out[i])
				n++
			}
		}
		if len(ref) != len(out) {
			lo := len(ref)
			if len(out) < lo {
				lo = len(out)
			}
			fmt.Printf("ZZ tail ref[%d:]=% x out[%d:]=% x\n", lo, ref[lo:], lo, out[lo:])
		}
		t.Fatalf("ZZ re-encode differs single-threaded")
	}
}
