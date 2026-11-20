package tests

// Scratch CI probe (debug/ci-probe only, never merge): dump exactly how the
// matrix3.bin round-trip differs on linux/amd64.
import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"testing"

	gen1 "pbwire/tests/gen/matrix3"
)

func zzRef(t *testing.T) []byte {
	ref, err := os.ReadFile(filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "c", "tests", "data", "matrix3.bin"))
	if err != nil {
		t.Fatal(err)
	}
	return ref
}

func zzDump(ref, out []byte) {
	fmt.Printf("ZZ ref len=%d out len=%d\n", len(ref), len(out))
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
}

func TestZZDebugRoundTrip(t *testing.T) {
	fmt.Printf("ZZ gmaxprocs=%d ncores=%d\n", runtime.GOMAXPROCS(0), runtime.NumCPU())
	ref := zzRef(t)
	m := &gen1.Scalars3{}
	if err := m.Unmarshal(ref); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	out, err := m.Marshal()
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	zzDump(ref, out)
	if !bytes.Equal(out, ref) {
		t.Fatalf("ZZ re-encode differs single-shot")
	}
}

// Same op, one goroutine, 400 iterations: fails => cross-call pollution
// (hint/arena/accumulator residue), passes => needs true concurrency.
func TestZZDebugSerial400(t *testing.T) {
	ref := zzRef(t)
	for k := 0; k < 400; k++ {
		m := &gen1.Scalars3{}
		if err := m.Unmarshal(ref); err != nil {
			t.Fatalf("iter %d unmarshal: %v", k, err)
		}
		out, err := m.Marshal()
		if err != nil {
			t.Fatalf("iter %d marshal: %v", k, err)
		}
		if !bytes.Equal(out, ref) {
			fmt.Printf("ZZ serial fails at iter %d\n", k)
			zzDump(ref, out)
			t.Fatalf("ZZ serial re-encode differs")
		}
	}
	fmt.Printf("ZZ serial 400 iters clean\n")
}

// 8 goroutines but serialized by a test-side mutex: passes => the codec
// lock is not covering some shared state (true race); fails => pollution
// visible even without overlap.
func TestZZDebugLocked8x50(t *testing.T) {
	ref := zzRef(t)
	var mu sync.Mutex
	var wg sync.WaitGroup
	errCh := make(chan string, 64)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for k := 0; k < 50; k++ {
				mu.Lock()
				m := &gen1.Scalars3{}
				uerr := m.Unmarshal(ref)
				var out []byte
				var merr error
				if uerr == nil {
					out, merr = m.Marshal()
				}
				mu.Unlock()
				if uerr != nil {
					errCh <- fmt.Sprintf("unmarshal: %v", uerr)
					return
				}
				if merr != nil {
					errCh <- fmt.Sprintf("marshal: %v", merr)
					return
				}
				if !bytes.Equal(out, ref) {
					errCh <- "locked re-encode differs"
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errCh)
	for msg := range errCh {
		t.Fatal(msg)
	}
	fmt.Printf("ZZ locked 8x50 clean\n")
}
