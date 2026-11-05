// The C example (examples/addressbook.c), in Swift, through the generated
// structs: assign fields, encode, decode, compare. The encoded bytes
// must equal the pinned reference examples/addressbook.bin.
//
// Run it with `swift run Roundtrip` (or `./build.sh example`) from
// bindings/swift.
import AddressbookGen
import Foundation

private func check(_ condition: Bool, _ label: String) {
    guard condition else {
        fputs("roundtrip: FAILED: \(label)\n", stderr)
        exit(1)
    }
}

private func phone(_ number: String, _ kind: Tutorial_Person_PhoneType) -> Tutorial_Person_PhoneNumber {
    var p = Tutorial_Person_PhoneNumber()
    p.number = number
    p.type = kind.rawValue
    return p
}

private func build() -> Tutorial_AddressBook {
    var alice = Tutorial_Person()
    alice.name = "Alice"
    alice.id = 1
    alice.phones = [phone("555-0100", .mobile)]
    var bob = Tutorial_Person()
    bob.name = "Bob"
    bob.id = 2
    bob.email = "bob@example.com"
    bob.phones = [phone("555-0200", .home)]
    var book = Tutorial_AddressBook()
    book.people = [alice, bob]
    return book
}

// #filePath is the compiled source path, so the fixture resolves without
// depending on the working directory.
let sourceDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let bin = sourceDir
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("examples/addressbook.bin").path

let book = build()
let wire = try book.encode()

if FileManager.default.fileExists(atPath: bin) {
    let reference = try Data(contentsOf: URL(fileURLWithPath: bin))
    check(wire == reference, "encode does not match \(bin)")
}

let back = try Tutorial_AddressBook.decode(wire)
check(back.people.count == 2, "two people")
check(back.people[0].name == "Alice" && back.people[0].id == 1, "alice round trip")
check(back.people[1].email == "bob@example.com", "bob email round trip")
print("addressbook: \(wire.count) bytes, \(back.people.count) people")
for p in back.people {
    print("  \(p.name) (\(p.id)) \(p.email ?? "")")
    for ph in p.phones {
        print("    \(ph.number)")
    }
}
