//! Typed-conformance check for the Rust binding: build matrix3.proto's
//! `Scalars3` through the generated structs, encode, require the protoc
//! reference bytes, then decode those bytes back and compare field by field.
//!
//! Same flow as every other binding's typed test.

#[path = "gen/matrix3_pbwire.rs"]
mod matrix3;

use matrix3::{Color3, Inner3, Scalars3};
use pbwire::Schema;

const DESC: &str = "../c/tests/data/matrix3.desc";
const ORACLE: &[u8] = include_bytes!("../../c/tests/data/matrix3.bin");

fn build() -> Scalars3 {
    Scalars3 {
        i32: i32::MIN,
        i64: i64::MAX,
        u32: u32::MAX as i32,
        u64: -1, // u64::MAX bits
        s32: i32::MIN,
        s64: i64::MIN,
        fx32: 0xdeadbeefu32 as i32,
        fx64: 72623859790382856,
        sfx32: i32::MIN,
        sfx64: i64::MIN,
        f32: f32::INFINITY,
        f64: -123456.789,
        flag: true,
        str: "hello".into(),
        color: Color3::COLOR3_GREEN,
        inner: Some(Box::new(Inner3 {
            name: "kid".into(),
            value: 42,
        })),
        nums: vec![1, -5, 300],
        tags: vec!["a".into(), "bb".into()],
        inners: vec![
            Inner3 {
                name: "x".into(),
                value: 1,
            },
            Inner3 {
                name: "y".into(),
                value: -2,
            },
        ],
        has_opt_i32: true,
        opt_i32: -7,
        has_opt_str: true,
        opt_str: "opt".into(),
        flags: vec![true, false, true],
    }
}

#[test]
fn typed_encode_matches_protoc_and_decodes_back() {
    let schema = Schema::from_file(DESC).expect("descriptor set loads");
    let typed = build();

    let wire = typed.encode(&schema).expect("typed encode");
    assert_eq!(wire, ORACLE, "typed encode equals the protoc reference bytes");

    let back = Scalars3::decode(&schema, ORACLE).expect("typed decode");
    assert_eq!(back, typed, "decode returns the same struct");

    let empty = Scalars3::default().encode(&schema).expect("empty encode");
    assert!(empty.is_empty(), "empty typed message is empty");

    let zero = Scalars3 {
        has_opt_i32: true,
        opt_i32: 0,
        ..Scalars3::default()
    };
    let zero_back = Scalars3::decode(&schema, &zero.encode(&schema).unwrap()).unwrap();
    assert!(zero_back.has_opt_i32 && zero_back.opt_i32 == 0);

    let inner_ty = schema.message(Inner3::FULL_NAME).expect("inner type");
    let inner_wire = Inner3 {
        name: "kid".into(),
        value: 0,
    }
    .encode(&schema)
    .expect("inner encode");
    assert_eq!(
        Inner3::decode(&schema, &inner_wire).unwrap().name,
        "kid",
        "inner encodes standalone"
    );
    let _ = inner_ty;
}
