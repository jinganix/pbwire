package gen

import (
	"fmt"
	"strings"

	"pbwire-codegen/internal/model"
)

// kotlinFiles emits the thin Kotlin layer over the Java facade: one inline
// DSL constructor per message (the shape official protobuf-kotlin generates,
// `scalars3 { setI32(1) }`), while every type, enum and codec stays the
// Java class. The wire format stays in the C core; there is no second model.
// Message and enum references resolve to the Java classes in the default
// package, which this file shares (no package declaration).
func kotlinFiles(f *model.File) []File {
	var b strings.Builder
	b.WriteString(header(f, "Thin Kotlin DSL over the Java facade classes: one inline builder per message."))
	b.WriteString("@file:Suppress(\"unused\")\n\n")
	for _, m := range f.Messages {
		cls := className(f, m.Name)
		fmt.Fprintf(&b, "/** %s: protobuf-kotlin-style DSL constructor, e.g. `%s { setName(\"x\") }`. */\n", m.Full, kotlinDsl(m.Name))
		fmt.Fprintf(&b, "inline fun %s(block: %s.Builder.() -> Unit): %s =\n", kotlinDsl(m.Name), cls, cls)
		fmt.Fprintf(&b, "    %s.newBuilder().apply(block).build()\n\n", cls)
	}
	return []File{{Name: base(f) + ".kt", Content: b.String()}}
}

// kotlinDsl is the DSL constructor name: lowerCamel of the local name with
// nesting underscores removed (Oneof1_Inner -> oneof1Inner).
func kotlinDsl(local string) string {
	parts := strings.Split(local, "_")
	first := strings.ToLower(parts[0][:1]) + parts[0][1:]
	return first + strings.Join(parts[1:], "")
}
