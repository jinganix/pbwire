// The C example (examples/addressbook.c), in Go, through the generated
// structs: assign fields, encode, decode, compare. The encoded bytes
// must equal the pinned reference examples/addressbook.bin.
//
//	go run ./examples/roundtrip.go (from bindings/go)
package main

import (
	"bytes"
	"fmt"
	"os"

	ab "pbwire/examples/gen/addressbook"
)

func phone(number string, kind ab.Person_PhoneType) *ab.Person_PhoneNumber {
	return &ab.Person_PhoneNumber{Number: number, Type: &kind}
}

func build() *ab.AddressBook {
	bobEmail := "bob@example.com"
	return &ab.AddressBook{People: []*ab.Person{
		{Name: "Alice", Id: 1, Phones: []*ab.Person_PhoneNumber{
			phone("555-0100", ab.Person_PhoneType_MOBILE),
		}},
		{Name: "Bob", Id: 2, Email: &bobEmail, Phones: []*ab.Person_PhoneNumber{
			phone("555-0200", ab.Person_PhoneType_HOME),
		}},
	}}
}

func main() {
	ref, err := os.ReadFile("../../examples/addressbook.bin")
	if err != nil {
		panic(err)
	}
	wire, err := build().Marshal()
	if err != nil {
		panic(err)
	}
	if !bytes.Equal(wire, ref) {
		panic("addressbook bytes differ from the pinned reference")
	}
	back := &ab.AddressBook{}
	if err := back.Unmarshal(ref); err != nil {
		panic(err)
	}
	fmt.Printf("addressbook: %d bytes, %d people\n", len(wire), len(back.GetPeople()))
	for _, p := range back.GetPeople() {
		fmt.Printf("  %s (%d) %s\n", p.GetName(), p.GetId(), p.GetEmail())
		for _, ph := range p.GetPhones() {
			fmt.Printf("    %s\n", ph.GetNumber())
		}
	}
}
