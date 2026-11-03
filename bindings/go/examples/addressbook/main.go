// AddressBook example (Go): load a FileDescriptorSet, encode, decode back.
//
// The schema comes from the same descriptor set the C and C++ examples use
// (`examples/addressbook.desc`, protoc --descriptor_set_out); nothing is
// generated for Go. The binding covers scalar fields today, so the example
// round-trips Person.name / Person.id and reports what else the type has.
//
// Run from bindings/go: `go run ./examples/addressbook` after `make shared`
// (and `make examples` once, to build examples/addressbook.desc).
package main

import (
	"fmt"
	"log"
	"path/filepath"
	"runtime"

	pbwire "pbwire/runtime"
)

var descPath = filepath.Join(sourceDir(), "..", "..", "..", "..", "examples", "addressbook.desc")

func main() {
	schema, err := pbwire.LoadFile(descPath)
	if err != nil {
		log.Fatal(err)
	}
	defer schema.Close()
	if schema.ABI() != 1 {
		log.Fatalf("libpbwire ABI %d, want 1", schema.ABI())
	}

	person, err := schema.Message("tutorial.Person")
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

func sourceDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(file)
}
