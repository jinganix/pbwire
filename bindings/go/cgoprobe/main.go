// Probe: what a cgo-based dynamic binding would pay per encode/decode,
// even before any protobuf work happens. Measures the cgo transition floor
// (nop call) and the cgo + memcpy pattern every FFI encode/decode needs
// (C writes bytes, the host copies them out), at the two extreme tiers
// from BENCHMARKS.md: 136 B and 74913 B.
//
// Not a full cgo-dynamic binding: it isolates the boundary so the existing
// `make go-bench` numbers (pure-Go static vs official vs vtproto) can be
// read against it. Run: cd bindings/go && go run ./cgoprobe
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#cgo LDFLAGS: ${SRCDIR}/../../../build/libpbwire.a
#include <stdlib.h>
#include <string.h>

void nop(void) {}
void fill136(unsigned char *dst, unsigned char *src) { memcpy(dst, src, 136); }
void fill74k(unsigned char *dst, unsigned char *src) { memcpy(dst, src, 74913); }
*/
import "C"

import (
	"fmt"
	"testing"
	"unsafe"
)

func mustMeasure(name string, f func(b *testing.B)) (float64, int) {
	r := testing.Benchmark(f)
	return float64(r.NsPerOp()), r.N
}

func main() {
	// Warm up the cgo path once so lazy trampolines settle.
	C.nop()

	nopNs, _ := mustMeasure("cgo-nop", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			C.nop()
		}
	})
	goNopNs, _ := mustMeasure("go-nop", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
		}
	})

	// C-side buffers, allocated once like a reused workspace.
	src136 := C.malloc(136)
	dst136 := C.malloc(136)
	src74k := C.malloc(74913)
	dst74k := C.malloc(74913)
	defer C.free(src136)
	defer C.free(dst136)
	defer C.free(src74k)
	defer C.free(dst74k)
	C.memset(src136, 0xAB, 136)
	C.memset(src74k, 0xAB, 74913)

	cgo136, _ := mustMeasure("cgo-copy136", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			C.fill136((*C.uchar)(dst136), (*C.uchar)(src136))
		}
	})
	cgo74k, _ := mustMeasure("cgo-copy74k", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			C.fill74k((*C.uchar)(dst74k), (*C.uchar)(src74k))
		}
	})

	// Pure-Go copies of the same sizes, for scale.
	gs, gd := make([]byte, 74913), make([]byte, 74913)
	for i := range gs {
		gs[i] = 0xAB
	}
	go136, _ := mustMeasure("go-copy136", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			copy(gd[:136], gs[:136])
		}
	})
	go74k, _ := mustMeasure("go-copy74k", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			copy(gd, gs)
		}
	})
	goBytes136, _ := mustMeasure("cgo-GoBytes136", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			_ = C.GoBytes(dst136, 136)
		}
	})
	goBytes74k, _ := mustMeasure("cgo-GoBytes74k", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			_ = C.GoBytes(dst74k, 74913)
		}
	})
	_ = unsafe.Pointer(nil)

	fmt.Printf("%-12s %12s\n", "op", "ns/op")
	fmt.Printf("%-12s %12.1f\n", "go-nop", goNopNs)
	fmt.Printf("%-12s %12.1f\n", "cgo-nop", nopNs)
	fmt.Printf("%-12s %12.1f\n", "go-copy136", go136)
	fmt.Printf("%-12s %12.1f\n", "cgo-copy136", cgo136)
	fmt.Printf("%-12s %12.1f\n", "go-copy74k", go74k)
	fmt.Printf("%-12s %12.1f\n", "cgo-copy74k", cgo74k)
	fmt.Printf("%-12s %12.1f\n", "GoBytes136", goBytes136)
	fmt.Printf("%-12s %12.1f\n", "GoBytes74k", goBytes74k)
	fmt.Printf("\ncgo floor per call (nop - go-nop): %.1f ns\n", nopNs-goNopNs)
	fmt.Printf("cgo memcpy premium at 136B: %.1f ns%s\n", cgo136-go136,
		" (transition dominates a ~60ns small encode)")
}
