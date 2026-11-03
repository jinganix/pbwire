// AddressBook example (Go): build a schema by hand, encode, decode back.
//
// The cgo binding covers scalar fields (the C ABI is the product); nested
// messages, repeated fields and schemas loaded from a descriptor set belong to
// the richer bindings. This example pins the part Go owns: the ABI round trip.
//
// Run from bindings/go: `go run ./examples/addressbook` after `make shared`.
package main

import (
	"fmt"
	"log"

	pbwire "pbwire/runtime"
)

func main() {
	schema := pbwire.New()
	defer schema.Close()
	if schema.ABI() != 1 {
		log.Fatalf("libpbwire ABI %d, want 1", schema.ABI())
	}

	person, err := schema.Message("Person", []pbwire.Field{
		{Name: "name", Number: 1, Type: pbwire.TString},
		{Name: "id", Number: 2, Type: pbwire.TInt32},
	})
	if err != nil {
		log.Fatal(err)
	}

	wire, err := person.Encode(map[uint32]pbwire.Value{
		1: {Bytes: []byte("alice"), IsStr: true},
		2: {Uint: 1234},
	})
	if err != nil {
		log.Fatal(err)
	}

	got, err := person.Decode(wire)
	if err != nil {
		log.Fatal(err)
	}
	if string(got[1].Bytes) != "alice" || got[2].Uint != 1234 {
		log.Fatalf("round trip mismatch: %+v", got)
	}
	fmt.Printf("%d bytes, name=%s id=%d\n", len(wire), got[1].Bytes, got[2].Uint)
}
