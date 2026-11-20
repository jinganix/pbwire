package tests

import (
	"fmt"
	"testing"
	"unsafe"

	gen1 "pbwire/tests/gen/matrix3"
	pbwrt "pbwire/runtime"
	"pbwire/runtime/layout"
)

func TestZZLayoutProbe(t *testing.T) {
	fmt.Printf("PROBE ptr=%d int=%d\n", unsafe.Sizeof(uintptr(0)), unsafe.Sizeof(int(0)))
	fmt.Printf("PROBE Scalars3 size=%d align=%d\n", unsafe.Sizeof(gen1.Scalars3{}), unsafe.Alignof(gen1.Scalars3{}))
	for _, f := range []struct {
		n string
		o uintptr
	}{
		{"I32", unsafe.Offsetof(gen1.Scalars3{}.I32)},
		{"I64", unsafe.Offsetof(gen1.Scalars3{}.I64)},
		{"Str", unsafe.Offsetof(gen1.Scalars3{}.Str)},
		{"Inner", unsafe.Offsetof(gen1.Scalars3{}.Inner)},
		{"Nums", unsafe.Offsetof(gen1.Scalars3{}.Nums)},
		{"Tags", unsafe.Offsetof(gen1.Scalars3{}.Tags)},
		{"Inners", unsafe.Offsetof(gen1.Scalars3{}.Inners)},
		{"OptI32", unsafe.Offsetof(gen1.Scalars3{}.OptI32)},
		{"OptStr", unsafe.Offsetof(gen1.Scalars3{}.OptStr)},
		{"Flags", unsafe.Offsetof(gen1.Scalars3{}.Flags)},
	} {
		fmt.Printf("PROBE off %s=%d\n", f.n, f.o)
	}
	fmt.Printf("PROBE Inner3 size=%d Name=%d Value=%d\n", unsafe.Sizeof(gen1.Inner3{}),
		unsafe.Offsetof(gen1.Inner3{}.Name), unsafe.Offsetof(gen1.Inner3{}.Value))
	regs := gen1.CshimRegistry()
	for _, m := range regs {
		fmt.Printf("PROBE reg %s size=%d rows=%d\n", m.Full, m.Size, len(m.Rows))
	}
	_ = layout.WireLen
	_ = pbwrt.NewCodecFromLayout
}
