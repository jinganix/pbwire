//! The C example (examples/addressbook.c), in Rust, through the generated
//! typed structs: assign fields, encode, decode, compare. The encoded bytes
//! must equal the pinned reference examples/addressbook.bin.
//!
//!   cd bindings/rust && cargo run --example roundtrip

#[path = "gen/addressbook_pbwire.rs"]
mod addressbook;

use addressbook::{person, AddressBook, Person};

const BIN: &str = "../../examples/addressbook.bin";

fn phone(number: &str, kind: person::PhoneType) -> person::PhoneNumber {
    person::PhoneNumber {
        number: number.into(),
        r#type: Some(kind as i32),
    }
}

fn build() -> AddressBook {
    AddressBook {
        people: vec![
            Person {
                name: "Alice".into(),
                id: 1,
                email: None,
                phones: vec![phone("555-0100", person::PhoneType::Mobile)],
            },
            Person {
                name: "Bob".into(),
                id: 2,
                email: Some("bob@example.com".into()),
                phones: vec![phone("555-0200", person::PhoneType::Home)],
            },
        ],
    }
}

fn main() {
    let wire = build().encode_to_vec();

    if let Ok(reference) = std::fs::read(BIN) {
        assert_eq!(wire, reference, "encode does not match {BIN}");
    }

    let book = AddressBook::decode(&wire).expect("decode");
    println!("addressbook: {} bytes, {} people", wire.len(), book.people.len());
    for person in &book.people {
        println!("  {} ({}) {}", person.name, person.id, person.email.as_deref().unwrap_or(""));
        for entry in &person.phones {
            println!("    {}", entry.number);
        }
    }
}
