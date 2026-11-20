package tests

import (
	"bytes"
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
