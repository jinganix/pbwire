//! The C example (examples/addressbook.c), in Rust, through the generated
//! typed structs: assign fields, encode, decode, compare. The encoded bytes
//! must equal the pinned reference examples/addressbook.bin.
//!
//!   cd bindings/rust && cargo run --example roundtrip

#[path = "gen/addressbook_pbwire.rs"]
mod addressbook;

use addressbook::{AddressBook, Person, PersonPhoneNumber, PersonPhoneType};
use pbwire::Schema;

const DESC: &str = "../../examples/addressbook.desc";
const BIN: &str = "../../examples/addressbook.bin";

fn phone(number: &str, kind: i32) -> PersonPhoneNumber {
    PersonPhoneNumber {
        number: number.into(),
        has_type: true,
        r#type: kind,
    }
}

fn build() -> AddressBook {
    AddressBook {
        people: vec![
            Person {
                name: "Alice".into(),
                id: 1,
                has_email: false,
                email: String::new(),
                phones: vec![phone("555-0100", PersonPhoneType::MOBILE)],
            },
            Person {
                name: "Bob".into(),
                id: 2,
                has_email: true,
                email: "bob@example.com".into(),
                phones: vec![phone("555-0200", PersonPhoneType::HOME)],
            },
        ],
    }
}

fn main() -> pbwire::Result<()> {
    let schema = Schema::from_file(DESC)?;
    let wire = build().encode(&schema)?;

    if let Ok(reference) = std::fs::read(BIN) {
        assert_eq!(wire, reference, "encode does not match {BIN}");
    }

    let book = AddressBook::decode(&schema, &wire)?;
    println!("addressbook: {} bytes, {} people", wire.len(), book.people.len());
    for person in &book.people {
        let email = if person.has_email { person.email.as_str() } else { "" };
        println!("  {} ({}) {}", person.name, person.id, email);
        for entry in &person.phones {
            println!("    {}", entry.number);
        }
    }
    Ok(())
}
