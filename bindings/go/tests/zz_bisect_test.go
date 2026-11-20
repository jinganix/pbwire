package tests

// Scratch CI probe (debug/ci-probe only, never merge): bisect encode vs
// decode. MarshalOnly shares one read-only message across goroutines (pure
// encode path). UnmarshalOnly unmarshals per goroutine and DeepEquals
// against a golden message built single-threaded (pure decode+copy path).
import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"

	gen1 "pbwire/tests/gen/matrix3"
)

func zzBisectRef(t *testing.T) []byte {
	ref, err := os.ReadFile(filepath.Join(filepath.Dir(callerFile(t)), "..", "..", "c", "tests", "data", "matrix3.bin"))
	if err != nil {
		t.Fatal(err)
	}
	return ref
}

func TestZZBisectMarshalOnly(t *testing.T) {
	ref := zzBisectRef(t)
	golden := &gen1.Scalars3{}
	if err := golden.Unmarshal(ref); err != nil {
		t.Fatalf("golden unmarshal: %v", err)
	}
	for round := 0; round < 10; round++ {
		var wg sync.WaitGroup
		errCh := make(chan string, 64)
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for k := 0; k < 50; k++ {
					out, err := golden.Marshal()
					if err != nil {
						errCh <- "marshal: " + err.Error()
						return
					}
					if !bytes.Equal(out, ref) {
						fmt.Printf("BISECT-M ref=%d out=%d\n", len(ref), len(out))
						n := 0
						for i := 0; i < len(ref) && i < len(out) && n < 10; i++ {
							if ref[i] != out[i] {
								fmt.Printf("BISECT-M diff at %d: ref=%02x out=%02x\n", i, ref[i], out[i])
								n++
							}
						}
						errCh <- "marshal-only differs"
						return
					}
				}
			}()
		}
		wg.Wait()
		close(errCh)
		for msg := range errCh {
			t.Fatalf("marshal round %d: %s", round, msg)
		}
	}
	fmt.Printf("BISECT marshal-only 10 rounds clean\n")
}

func TestZZBisectUnmarshalOnly(t *testing.T) {
	ref := zzBisectRef(t)
	golden := &gen1.Scalars3{}
	if err := golden.Unmarshal(ref); err != nil {
		t.Fatalf("golden unmarshal: %v", err)
	}
	for round := 0; round < 10; round++ {
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
					if !reflect.DeepEqual(m, golden) {
						fmt.Printf("BISECT-U deep mismatch: got=%+v\n", m)
						errCh <- "unmarshal-only differs"
						return
					}
				}
			}()
		}
		wg.Wait()
		close(errCh)
		for msg := range errCh {
			t.Fatalf("unmarshal round %d: %s", round, msg)
		}
	}
	fmt.Printf("BISECT unmarshal-only 10 rounds clean\n")
}
