//! Self-check against the pinned `protoc` bytes: build `structtest.Scalars3`
//! from `bindings/c/tests/data/matrix3.txt`, encode, and compare with
//! `matrix3.bin` (the same oracle the C binding uses). Then decode it back and
//! check the values survived, including shared-field repeated handling.

use pbwire::{Schema, Type};

const DESC: &str = "../c/tests/data/matrix3.desc";
const ORACLE: &[u8] = include_bytes!("../../c/tests/data/matrix3.bin");
const MSG: &str = "structtest.Scalars3";

fn fill<'a>(ty: &'a Type<'a>, inner_ty: &'a Type<'a>) -> pbwire::Message<'a> {
    let mut m = ty.new_message();
    m.set_i64(ty.field("i32").unwrap(), i32::MIN as i64)
        .set_i64(ty.field("i64").unwrap(), i64::MAX)
        .set_i64(ty.field("u32").unwrap(), u32::MAX as i64)
        .set_i64(ty.field("u64").unwrap(), -1) /* u64::MAX bits */
        .set_i64(ty.field("s32").unwrap(), i32::MIN as i64)
        .set_i64(ty.field("s64").unwrap(), i64::MIN)
        .set_i64(ty.field("fx32").unwrap(), 0xdeadbeef)
        .set_i64(ty.field("fx64").unwrap(), 72623859790382856)
        .set_i64(ty.field("sfx32").unwrap(), i32::MIN as i64)
        .set_i64(ty.field("sfx64").unwrap(), i64::MIN)
        .set_f32(ty.field("f32").unwrap(), f32::INFINITY)
        .set_f64(ty.field("f64").unwrap(), -123456.789)
        .set_bool(ty.field("flag").unwrap(), true)
        .set_str(ty.field("str").unwrap(), "hello")
        .set_i64(ty.field("color").unwrap(), 2)
        .set_i64(ty.field("opt_i32").unwrap(), -7)
        .set_str(ty.field("opt_str").unwrap(), "opt");
    for value in [1, -5, 300] {
        m.set_i64(ty.field("nums").unwrap(), value);
    }
    for tag in ["a", "bb"] {
        m.set_str(ty.field("tags").unwrap(), tag);
    }
    for (name, value) in [("x", 1), ("y", -2)] {
        m.set_message(ty.field("inners").unwrap())
            .set_str(inner_ty.field("name").unwrap(), name)
            .set_i64(inner_ty.field("value").unwrap(), value);
    }
    for flag in [true, false, true] {
        m.set_bool(ty.field("flags").unwrap(), flag);
    }
    m.set_message(ty.field("inner").unwrap())
        .set_str(inner_ty.field("name").unwrap(), "kid")
        .set_i64(inner_ty.field("value").unwrap(), 42);
    m
}

#[test]
fn encode_matches_protoc_oracle() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let ty = schema.message(MSG).expect("message type");
    let inner_ty = schema.message("structtest.Inner3").expect("inner type");
    let mut msg = fill(&ty, &inner_ty);

    let wire = msg.encode().expect("encode");
    assert_eq!(wire, ORACLE, "encoded bytes differ from protoc");

    /* The reusable-buffer path must produce the same bytes. */
    let mut buf = Vec::new();
    let encoded = msg.encode_to(&mut buf).expect("encode_to");
    assert_eq!(encoded, ORACLE);

    /* Repeated scalars append; a singular field is replaced. */
    msg.set_str(ty.field("str").unwrap(), "hello");
    assert_eq!(msg.encode().expect("encode"), ORACLE);
}

#[test]
fn decode_roundtrip() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let ty = schema.message(MSG).expect("message type");
    let inner_ty = schema.message("structtest.Inner3").expect("inner type");
    let decoded = ty.decode(ORACLE).expect("decode");

    assert_eq!(decoded.get_i64(ty.field("i32").unwrap()), i32::MIN as i64);
    assert!(decoded.get_bool(ty.field("flag").unwrap()));
    assert_eq!(decoded.get_str(ty.field("str").unwrap()), Some("hello"));
    assert_eq!(decoded.get_f64(ty.field("f64").unwrap()), -123456.789);
    assert_eq!(decoded.get_i64(ty.field("color").unwrap()), 2);
    assert!(decoded.is_initialized());

    let inner = decoded.get_message(ty.field("inner").unwrap()).expect("inner");
    assert_eq!(inner.get_str(inner_ty.field("name").unwrap()), Some("kid"));
    assert_eq!(inner.get_i64(inner_ty.field("value").unwrap()), 42);

    let nums = decoded.value(ty.field("nums").unwrap()).expect("nums");
    assert_eq!(nums.len(), 3);
    assert_eq!(
        (0..nums.len())
            .map(|i| nums.at(i).expect("item").as_i64())
            .collect::<Vec<_>>(),
        [1, -5, 300]
    );

    let tags = decoded.value(ty.field("tags").unwrap()).expect("tags");
    assert_eq!(tags.at(1).expect("item").as_str(), Some("bb"));

    let inners = decoded.value(ty.field("inners").unwrap()).expect("inners");
    assert_eq!(inners.len(), 2);
    assert_eq!(
        inners
            .at(1)
            .expect("item")
            .as_message()
            .unwrap()
            .get_i64(inner_ty.field("value").unwrap()),
        -2
    );

    /* Byte-exact re-encode proves unknown/presence state survived. */
    assert_eq!(decoded.encode().expect("encode"), ORACLE);
}

#[test]
fn unknown_fields_survive() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let ty = schema.message("structtest.Inner3").expect("message type");

    /* field 99, wire type 0 = varint 7: not in the schema. */
    let wire = [0x98, 0x06, 0x07];
    let decoded = ty.decode(&wire).expect("decode");
    assert_eq!(decoded.encode().expect("encode"), wire);
}

#[test]
fn bad_input_is_an_error() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let ty = schema.message(MSG).expect("message type");

    /* Truncated tag: refused, not a crash. */
    assert!(ty.decode(&[0x80]).is_err());
    assert!(schema.message("structtest.Nope").is_err());
    assert!(ty.field("nope").is_err());
}

/* ---------------------------------------------------------------- edge cases */

const EDGE_DESC: &str = "../../tests/data/edge.desc";

/// The cases a binding can get wrong even when the core is right: payloads with
/// embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an empty
/// repeated element, proto3 optional presence with an empty value, and oneof
/// switching. `tests/data/edge.bin` is `protoc --encode`.
#[test]
fn edge_payload_matches_protoc() {
    let oracle = std::fs::read("../../tests/data/edge.bin").expect("read edge.bin");
    let schema = Schema::from_file(EDGE_DESC).expect("load edge descriptor set");
    let ty = schema.message("edge.Edge").expect("Edge");
    let inner_ty = schema.message("edge.Inner").expect("Inner");

    let mut msg = ty.new_message();
    msg.set_bytes(ty.field("raw").unwrap(), &[0x00, 0x01, 0xff])
        .set_str(ty.field("text").unwrap(), "héllo ☃")
        .set_i64(ty.field("sign").unwrap(), -3)
        .set_str(ty.field("maybe").unwrap(), "")
        .set_i64(ty.field("pick_num").unwrap(), 5) /* oneof: replaced below */
        .set_str(ty.field("pick_str").unwrap(), "one");
    for tag in ["a", "", "b"] {
        msg.set_str(ty.field("tags").unwrap(), tag);
    }
    msg.set_bytes(ty.field("blobs").unwrap(), &[0x00]);
    msg.set_bytes(ty.field("blobs").unwrap(), &[]);
    msg.set_message(ty.field("inner").unwrap())
        .set_str(inner_ty.field("tag").unwrap(), "kid")
        .set_i64(inner_ty.field("n").unwrap(), -1);

    assert_eq!(msg.encode().unwrap(), oracle, "edge payload differs from protoc");

    let decoded = ty.decode(&oracle).expect("decode");
    assert_eq!(decoded.get_bytes(ty.field("raw").unwrap()), Some(&[0x00, 0x01, 0xff][..]));
    assert_eq!(decoded.get_str(ty.field("text").unwrap()), Some("héllo ☃"));
    assert_eq!(decoded.get_i64(ty.field("sign").unwrap()), -3);

    let tags = decoded.value(ty.field("tags").unwrap()).expect("tags");
    assert_eq!(tags.len(), 3);
    assert_eq!(tags.at(1).expect("item").as_str(), Some(""));

    let blobs = decoded.value(ty.field("blobs").unwrap()).expect("blobs");
    assert_eq!(blobs.at(0).expect("item").as_bytes(), Some(&[0x00][..]));
    assert_eq!(blobs.at(1).expect("item").as_bytes(), Some(&[][..]));

    let inner = decoded.get_message(ty.field("inner").unwrap()).expect("inner");
    assert_eq!(inner.get_i64(inner_ty.field("n").unwrap()), -1);

    /* proto3 optional: present, and present with an empty value. */
    assert!(decoded.has(ty.field("maybe").unwrap()));
    assert_eq!(decoded.get_str(ty.field("maybe").unwrap()), Some(""));

    /* oneof: only the last member set is there. */
    assert!(decoded.has(ty.field("pick_str").unwrap()));
    assert!(!decoded.has(ty.field("pick_num").unwrap()));

    assert_eq!(decoded.encode().expect("encode"), oracle, "decode -> encode is byte-exact");
}

/// A default message is zero bytes both ways: the common case for proto3 and
/// the one place a binding can hand a null pointer to a zero-length slice.
#[test]
fn empty_message_roundtrip() {
    let schema = Schema::from_file(EDGE_DESC).expect("load edge descriptor set");
    let ty = schema.message("edge.Edge").expect("Edge");

    let empty = ty.new_message();
    assert_eq!(empty.encode().expect("encode"), Vec::<u8>::new());

    let mut buf = Vec::new();
    assert_eq!(empty.encode_to(&mut buf).expect("encode_to"), Vec::<u8>::new());

    let decoded = ty.decode(&[]).expect("decode empty");
    assert!(!decoded.has(ty.field("text").unwrap()));
    assert!(decoded.is_initialized()); /* no proto2 required fields */
    assert_eq!(decoded.encode().expect("encode"), Vec::<u8>::new());
}

/// A value of the wrong kind is refused, not written: the core validates, the
/// binding must not swallow it or free something it still owns.
#[test]
#[should_panic(expected = "type mismatch")]
fn wrong_typed_value_is_rejected() {
    let schema = Schema::from_file(EDGE_DESC).expect("load edge descriptor set");
    let ty = schema.message("edge.Edge").expect("Edge");
    ty.new_message().set_str(ty.field("pick_num").unwrap(), "not a number");
}

/// set_message on a scalar field is an error, not a silent no-op.
#[test]
#[should_panic(expected = "not a message field")]
fn set_message_on_scalar_is_rejected() {
    let schema = Schema::from_file(EDGE_DESC).expect("load edge descriptor set");
    let ty = schema.message("edge.Edge").expect("Edge");
    ty.new_message().set_message(ty.field("text").unwrap());
}
