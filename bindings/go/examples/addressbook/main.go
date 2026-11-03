// AddressBook example (Go): generated codecs, no schema at run time.
//
// The message types and their size/encode/decode methods are generated from
// examples/addressbook.proto by protoc-gen-pbwire-go; this file only fills the
// structs and prints the round trip. Regenerate with
// `make -C bindings/go/plugin regen`, run with
//
//	cd bindings/go && go run ./examples/addressbook
package main

import (
	"fmt"
	"log"

	tutorial "pbwire/examples/addressbook/tutorial"
)

func main() {
	book := &tutorial.AddressBook{People: []tutorial.Person{{
		Name:  ptr("alice"),
		Id:    ptr(int32(1234)),
		Email: ptr("alice@example.com"),
		Phones: []tutorial.Person_PhoneNumber{
			{Number: ptr("555-1212"), Type: ptr(tutorial.Person_PhoneType_HOME)},
			{Number: ptr("555-3434"), Type: ptr(tutorial.Person_PhoneType_WORK)},
		},
	}}}

	wire, err := book.Marshal()
	if err != nil {
		log.Fatal(err)
	}

	var back tutorial.AddressBook
	if err := back.Unmarshal(wire); err != nil {
		log.Fatal(err)
	}
	person := back.People[0]
	fmt.Printf("%d bytes, name=%s id=%d phones=%d first=%s/%d\n",
		len(wire), *person.Name, *person.Id, len(person.Phones),
		*person.Phones[0].Number, int32(*person.Phones[0].Type))
}

func ptr[T any](v T) *T { return &v }
