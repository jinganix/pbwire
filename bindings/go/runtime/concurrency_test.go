package runtime_test

// Concurrent codec contract, stated once:
//
//   - Tables are read-only after NewTable; the only shared write is the
//     capacity hint behind an atomic, which settles once the size is
//     stable. Sharing one table across goroutines is safe.
//   - Encoder, Decoder and Arena are per-op values and are never shared.
//   - Concurrent Marshal of one shared message is safe while nobody writes
//     to it: encode only reads the struct (fast-path loads, getter
//     closures). Concurrent Unmarshal needs a fresh target per op, because
//     decode writes.
//
// These tests hammer the thin-model path the benchmarks time (shared table,
// per-goroutine or shared messages) and fail under -race if any of that is
// wrong. They assert byte identity, not speed.

import (
	"bytes"
	"errors"
	"sync"
	"testing"

	"pbwire/tests/gen/matrix3"
)

func concurrentFixture() *matrix3.Scalars3 {
	return &matrix3.Scalars3{
		I32: -2147483648, I64: 9223372036854775807,
		U32: 4294967295, U64: 18446744073709551615,
		S32: -2147483648, S64: -9223372036854775808,
		Fx32: 0xdeadbeef, Fx64: 72623859790382856,
		Sfx32: -2147483648, Sfx64: -9223372036854775808,
		F32: 1.5, F64: -123456.789, Flag: true,
		Str:   "hello",
		Color: matrix3.Color3_COLOR3_GREEN,
		Inner: &matrix3.Inner3{Name: "kid", Value: 42},
		Nums:  []int32{-3, -2, -1, 0, 1, 2, 300},
		Tags:  []string{"tag", "tag"},
		Inners: []*matrix3.Inner3{
			{Name: "grandkid", Value: 1},
			{Name: "grandkid", Value: 2},
		},
		Flags:  []bool{true, false, true},
		OptI32: i32ptr(-7),
		OptStr: strptr("opt"),
	}
}

func i32ptr(v int32) *int32 { return &v }

func strptr(v string) *string { return &v }

// TestConcurrentEncodeSharedTable mirrors the server pattern behind the
// *-par bench rows: one table (inside the generated Marshal), one message
// per goroutine, all racing. Every output must equal the serial bytes.
func TestConcurrentEncodeSharedTable(t *testing.T) {
	want, err := concurrentFixture().Marshal()
	if err != nil {
		t.Fatal(err)
	}
	const goroutines = 8
	const iters = 200
	var wg sync.WaitGroup
	errs := make(chan error, goroutines)
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			m := concurrentFixture()
			for i := 0; i < iters; i++ {
				got, err := m.Marshal()
				if err != nil {
					errs <- err
					return
				}
				if !bytes.Equal(got, want) {
					errs <- errors.New("concurrent encode diverged from serial bytes")
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
}

// TestConcurrentEncodeSharedMessage proves the read-only share: one message
// read by every goroutine at once. Safe only while nobody writes to it,
// which is exactly the benchmark's shared-nothing alternative.
func TestConcurrentEncodeSharedMessage(t *testing.T) {
	m := concurrentFixture()
	want, err := m.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	const goroutines = 8
	const iters = 200
	var wg sync.WaitGroup
	errs := make(chan error, goroutines)
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < iters; i++ {
				got, err := m.Marshal()
				if err != nil {
					errs <- err
					return
				}
				if !bytes.Equal(got, want) {
					errs <- errors.New("shared-message concurrent encode diverged")
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
}

// TestConcurrentDecodeSharedTable races fresh-target decodes over one table
// and checks the fields that the fast store path writes.
func TestConcurrentDecodeSharedTable(t *testing.T) {
	wire, err := concurrentFixture().Marshal()
	if err != nil {
		t.Fatal(err)
	}
	const goroutines = 8
	const iters = 200
	var wg sync.WaitGroup
	errs := make(chan error, goroutines)
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < iters; i++ {
				back := &matrix3.Scalars3{}
				if err := back.Unmarshal(wire); err != nil {
					errs <- err
					return
				}
				if back.I32 != -2147483648 || back.Str != "hello" ||
					back.Inner.GetName() != "kid" || len(back.Nums) != 7 ||
					back.GetOptI32() != -7 || back.Flags[1] {
					errs <- errors.New("concurrent decode lost fields")
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
}
