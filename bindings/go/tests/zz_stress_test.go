package tests

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"testing"

	gen1 "pbwire/tests/gen/matrix3"
)

func TestZZStress100(t *testing.T) {
	ref, err := os.ReadFile(filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "c", "tests", "data", "matrix3.bin"))
	if err != nil {
		t.Fatal(err)
	}
	for round := 0; round < 100; round++ {
		var wg sync.WaitGroup
		errCh := make(chan string, 64)
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for k := 0; k < 50; k++ {
					m := &gen1.Scalars3{}
					if err := m.Unmarshal(ref); err != nil {
						errCh <- "unmarshal: " + err.Error()
						return
					}
					out, err := m.Marshal()
					if err != nil {
						errCh <- "marshal: " + err.Error()
						return
					}
					if !bytes.Equal(out, ref) {
						// Scratch-only dump on the failure path.
						fmt.Printf("STRESS round=%d ref=%d out=%d\n", round, len(ref), len(out))
						n := 0
						for i := 0; i < len(ref) && i < len(out) && n < 10; i++ {
							if ref[i] != out[i] {
								fmt.Printf("STRESS diff at %d: ref=%02x out=%02x\n", i, ref[i], out[i])
								n++
							}
						}
						errCh <- "re-encode differs"
						return
					}
				}
			}()
		}
		wg.Wait()
		close(errCh)
		for msg := range errCh {
			t.Fatalf("round %d: %s", round, msg)
		}
	}
}
