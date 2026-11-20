package tests

// Scratch CI probe (debug/ci-probe only, never merge): byte-identical copies
// of TestModelConcurrentUse to test whether failure follows file/test order.
import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"testing"

	gen1 "pbwire/tests/gen/matrix3"
)

func aaLoop(t *testing.T) {
	ref, err := os.ReadFile(filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "c", "tests", "data", "matrix3.bin"))
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	type hit struct {
		k, g   int
		second string
		out    []byte
	}
	hitCh := make(chan hit, 64)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for k := 0; k < 50; k++ {
				m := &gen1.Scalars3{}
				if err := m.Unmarshal(ref); err != nil {
					hitCh <- hit{k: k, g: g, second: "unmarshal: " + err.Error()}
					return
				}
				out, err := m.Marshal()
				if err != nil {
					hitCh <- hit{k: k, g: g, second: "marshal: " + err.Error()}
					return
				}
				if !bytes.Equal(out, ref) {
					second := "marshal-err"
					if out2, err2 := m.Marshal(); err2 == nil {
						second = "dirty"
						if bytes.Equal(out2, ref) {
							second = "clean"
						}
					}
					hitCh <- hit{k: k, g: g, second: second, out: out}
					return
				}
			}
		}(i)
	}
	wg.Wait()
	close(hitCh)
	for h := range hitCh {
		if h.out != nil {
			fmt.Printf("AA mismatch goroutine=%d k=%d second-marshal=%s\n", h.g, h.k, h.second)
			zzDump(ref, h.out)
			t.Fatal("re-encode differs")
		}
		t.Fatal(h.second)
	}
}

func TestAaRace1(t *testing.T) { aaLoop(t) }

func TestAaRace2(t *testing.T) { aaLoop(t) }

func TestAaRace3(t *testing.T) { aaLoop(t) }
