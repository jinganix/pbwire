//! Codec gate for the pure-Rust core: the pinned `matrix3` fixture bytes in
//! both directions, unknown retention (in input order, wrong-wire-type
//! included), groups, packed runs, oneofs, presence, the 64-bit edges and the
//! limits. Every byte string here is either the C binding's pinned reference or
//! a hand-written wire image with the shape the assertion is about.

use std::path::PathBuf;

use pbwire::core::{
    CoreError, Kind, Limits, Message, MessageType, Presence, Schema, Value, MAX_DEPTH,
};

fn data_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../c/tests/data")
}

fn fixture(name: &str) -> Vec<u8> {
    let path = data_dir().join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn load(file: &str) -> Schema {
    let path = data_dir().join(file);
    Schema::from_file(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn type_of(file: &str, name: &str) -> MessageType {
    load(file).message(name).unwrap_or_else(|e| panic!("{file}: {e}"))
}

fn status_of<T>(r: pbwire::core::Result<T>) -> i32 {
    match r {
        Ok(_) => 0,
        Err(e) => e.status(),
    }
}

fn varint(mut v: u64) -> Vec<u8> {
    let mut out = Vec::new();
    while v > 0x7f {
        out.push((v & 0x7f) as u8 | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
    out
}

fn ld(fnum: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = varint(fnum as u64 * 8 + 2);
    out.extend(varint(payload.len() as u64));
    out.extend_from_slice(payload);
    out
}

fn vint(fnum: u32, v: u64) -> Vec<u8> {
    let mut out = varint(fnum as u64 * 8);
    out.extend(varint(v));
    out
}

fn start_group(fnum: u32) -> Vec<u8> {
    varint(fnum as u64 * 8 + 3)
}

fn end_group(fnum: u32) -> Vec<u8> {
    varint(fnum as u64 * 8 + 4)
}

/// The `structtest.Scalars3` fixture, built through the public tree API.
fn build(ty: &MessageType) -> Message {
    let mut m = ty.new_message();
    m.set_i64("i32", i32::MIN as i64).unwrap();
    m.set_i64("i64", i64::MAX).unwrap();
    m.set_u64("u32", u32::MAX as u64).unwrap();
    m.set_u64("u64", u64::MAX).unwrap();
    m.set_i64("s32", i32::MIN as i64).unwrap();
    m.set_i64("s64", i64::MIN).unwrap();
    m.set_u64("fx32", 0xdead_beef).unwrap();
    m.set_u64("fx64", 72623859790382856).unwrap();
    m.set_i64("sfx32", i32::MIN as i64).unwrap();
    m.set_i64("sfx64", i64::MIN).unwrap();
    m.set_f32("f32", f32::INFINITY).unwrap();
    m.set_f64("f64", -123456.789).unwrap();
    m.set_bool("flag", true).unwrap();
    m.set_str("str", "hello").unwrap();
    m.set_i64("color", 2).unwrap();
    m.set_i64("opt_i32", -7).unwrap();
    m.set_str("opt_str", "opt").unwrap();
    {
        let inner = m.set_message("inner").unwrap();
        inner.set_str("name", "kid").unwrap();
        inner.set_i64("value", 42).unwrap();
    }
    for n in [1i64, -5, 300] {
        m.push("nums", Value::I64(n)).unwrap();
    }
    for t in ["a", "bb"] {
        m.push("tags", Value::Bytes(t.as_bytes().to_vec())).unwrap();
    }
    for (name, value) in [("x", 1i64), ("y", -2)] {
        let inner = m.push_message("inners").unwrap();
        inner.set_str("name", name).unwrap();
        inner.set_i64("value", value).unwrap();
    }
    for v in [true, false, true] {
        m.push("flags", Value::Bool(v)).unwrap();
    }
    m
}

#[test]
fn encode_matches_the_reference() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let reference = fixture("matrix3.bin");
    let got = ty.encode(&build(&ty)).expect("encode");
    assert_eq!(got, reference, "core encode != protoc reference bytes");
    assert_eq!(reference.len(), 192);
}

#[test]
fn decode_reads_every_field_and_re_encodes() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let reference = fixture("matrix3.bin");
    let m = ty.decode(&reference).expect("decode");
    assert_eq!(m.get_i64("i32").unwrap(), Some(i32::MIN as i64));
    assert_eq!(m.get_i64("i64").unwrap(), Some(i64::MAX));
    assert_eq!(m.get_u64("u32").unwrap(), Some(u32::MAX as u64));
    assert_eq!(m.get_u64("u64").unwrap(), Some(u64::MAX));
    assert_eq!(m.get_i64("s32").unwrap(), Some(i32::MIN as i64));
    assert_eq!(m.get_i64("s64").unwrap(), Some(i64::MIN));
    assert_eq!(m.get_u64("fx32").unwrap(), Some(0xdead_beef));
    assert_eq!(m.get_u64("fx64").unwrap(), Some(72623859790382856));
    assert_eq!(m.get_i64("sfx32").unwrap(), Some(i32::MIN as i64));
    assert_eq!(m.get_i64("sfx64").unwrap(), Some(i64::MIN));
    assert_eq!(m.get_f32("f32").unwrap(), Some(f32::INFINITY));
    assert_eq!(m.get_f64("f64").unwrap(), Some(-123456.789));
    assert_eq!(m.get_bool("flag").unwrap(), Some(true));
    assert_eq!(m.get_str("str").unwrap().as_deref(), Some("hello"));
    assert_eq!(m.get_i64("color").unwrap(), Some(2));
    assert_eq!(m.get_i64("opt_i32").unwrap(), Some(-7));
    assert_eq!(m.get_str("opt_str").unwrap().as_deref(), Some("opt"));
    let inner = m.get_message("inner").unwrap().expect("inner present");
    assert_eq!(inner.get_str("name").unwrap().as_deref(), Some("kid"));
    assert_eq!(inner.get_i64("value").unwrap(), Some(42));
    assert_eq!(m.len("nums").unwrap(), 3);
    assert_eq!(m.items("nums").unwrap()[0].as_i64(), Some(1));
    assert_eq!(m.items("nums").unwrap()[1].as_i64(), Some(-5));
    assert_eq!(m.items("nums").unwrap()[2].as_i64(), Some(300));
    assert_eq!(m.len("tags").unwrap(), 2);
    assert_eq!(m.items("tags").unwrap()[1].as_bytes(), Some(&b"bb"[..]));
    assert_eq!(m.len("inners").unwrap(), 2);
    assert_eq!(m.len("flags").unwrap(), 3);
    assert_eq!(m.items("flags").unwrap()[1].as_bool(), Some(false));
    assert_eq!(ty.encode(&m).expect("re-encode"), reference);
    // The round trip is byte-exact, so a second one is too.
    assert_eq!(ty.encode(&ty.decode(&reference).unwrap()).unwrap(), reference);
}

#[test]
fn u64_and_i64_edges_round_trip() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let mut m = ty.new_message();
    m.set_u64("u64", u64::MAX).unwrap();
    m.set_i64("i64", i64::MIN).unwrap();
    m.set_u64("fx64", u64::MAX).unwrap();
    m.set_i64("sfx64", i64::MIN).unwrap();
    m.set_i64("s64", i64::MIN).unwrap();
    let wire = ty.encode(&m).unwrap();
    let back = ty.decode(&wire).unwrap();
    assert_eq!(back.get_u64("u64").unwrap(), Some(u64::MAX));
    assert_eq!(back.get_i64("i64").unwrap(), Some(i64::MIN));
    assert_eq!(back.get_u64("fx64").unwrap(), Some(u64::MAX));
    assert_eq!(back.get_i64("sfx64").unwrap(), Some(i64::MIN));
    assert_eq!(back.get_i64("s64").unwrap(), Some(i64::MIN));
    assert_eq!(ty.encode(&back).unwrap(), wire);
    // sint64 zigzag: i64::MIN is the widest encoding (10 bytes unsigned).
    let s64 = ty.field("s64").unwrap().number;
    let w = vint(s64, u64::MAX);
    assert_eq!(ty.decode(&w).unwrap().get_i64("s64").unwrap(), Some(i64::MIN));
}

#[test]
fn unknown_fields_are_retained_in_input_order() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let mut wire = fixture("matrix3.bin");
    // A known field, then two unknowns: varint and length-delimited.
    let mut tail = vint(99, 5);
    tail.extend(ld(100, b"opaque"));
    wire.extend_from_slice(&tail);
    let m = ty.decode(&wire).expect("decode with unknowns");
    assert_eq!(m.unknown().len(), 2, "two retained unknowns");
    assert_eq!(m.unknown()[0], vint(99, 5));
    assert_eq!(m.unknown()[1], ld(100, b"opaque"));
    let re = ty.encode(&m).expect("re-encode");
    assert_eq!(re, wire, "unknowns re-encode verbatim and last");
    // An unknown group is validated to its end tag, then kept.
    let mut wire2 = fixture("matrix3.bin");
    let mut group = start_group(101);
    group.extend(vint(1, 1));
    group.extend(end_group(101));
    wire2.extend_from_slice(&group);
    let m2 = ty.decode(&wire2).expect("decode with an unknown group");
    assert_eq!(m2.unknown(), &[group]);
    assert_eq!(ty.encode(&m2).unwrap(), wire2);
    // A mismatched end tag inside an unknown group fails the whole decode...
    let mut bad = start_group(101);
    bad.extend(end_group(102));
    assert!(ty.decode(&bad).is_err(), "mismatched end-group");
    // ...and an end tag with no open group is stray (status 1).
    assert_eq!(status_of(ty.decode(&end_group(102))), 1, "stray end-group");
}

#[test]
fn wrong_wire_type_is_an_unknown() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    // Field 1 (`i32`) declared FIXED32: a wrong wire type, kept as an unknown.
    let wire = vec![0x0d, 1, 2, 3, 4];
    let m = ty.decode(&wire).expect("decode");
    assert!(m.get("i32").unwrap().is_none(), "not read as i32");
    assert_eq!(m.unknown(), &[wire.clone()]);
    assert_eq!(ty.encode(&m).unwrap(), wire);
}

#[test]
fn groups_round_trip() {
    let ty = type_of("grouptest.desc", "structtest.GroupTest");
    let reference = fixture("grouptest.bin");
    let m = ty.decode(&reference).expect("decode group fixture");
    assert_eq!(ty.encode(&m).unwrap(), reference);
    let inner = m.get_message("inner").unwrap().expect("inner group");
    assert!(inner.get_str("s").unwrap().is_some());
    // Two occurrences of a singular group merge into one, like the core.
    let mut a = start_group(1);
    a.extend(ld(3, b"one"));
    a.extend(end_group(1));
    let mut b = start_group(1);
    b.extend(vint(2, 7));
    b.extend(end_group(1));
    let mut wire = a.clone();
    wire.extend_from_slice(&b);
    let merged = ty.decode(&wire).expect("merge groups");
    let inner = merged.get_message("inner").unwrap().expect("merged group");
    assert_eq!(inner.get_str("s").unwrap().as_deref(), Some("one"));
    assert_eq!(inner.get_i64("x").unwrap(), Some(7));
    // A group body cut off before its end tag fails.
    let mut cut = start_group(1);
    cut.extend(ld(3, b"one"));
    assert_eq!(status_of(ty.decode(&cut)), 1);
}

#[test]
fn packed_and_unpacked_forms_are_both_accepted() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    // `nums` is packed in the schema; the unpacked element form still parses.
    let nums = ty.field("nums").unwrap().number;
    let mut wire = vint(nums, 1);
    wire.extend(vint(nums, 2));
    let m = ty.decode(&wire).expect("unpacked elements");
    assert_eq!(m.len("nums").unwrap(), 2);
    // And the packed form under the same field number.
    let mut payload = Vec::new();
    payload.extend(varint(1));
    payload.extend(varint(u64::MAX)); // -1 as int32 truncates to -1
    let packed = ld(nums, &payload);
    let m = ty.decode(&packed).expect("packed run");
    let items: Vec<i32> = m.items("nums").unwrap().iter().map(|v| v.as_i64().unwrap() as i32).collect();
    assert_eq!(items, vec![1, -1]);
    assert_eq!(ty.encode(&m).unwrap(), packed);
    // An empty packed run adds nothing.
    let empty = ld(nums, b"");
    let m = ty.decode(&empty).expect("empty packed run");
    assert!(!m.has("nums").unwrap(), "an empty run leaves the field absent");
    // A packed run that overruns its own length fails.
    let mut bad = varint(nums as u64 * 8 + 2);
    bad.extend(varint(1));
    bad.push(0x80); // one byte of a varint that never terminates
    assert!(ty.decode(&bad).is_err(), "packed overrun");
    // A proto2 unpacked field accepts a packed run too.
    let ty2 = type_of("matrix2.desc", "structtest.Scalars2");
    let nums = ty2.field("nums").unwrap();
    assert!(nums.repeated && !nums.packed);
    let mut payload = Vec::new();
    payload.extend(varint(4));
    let mut wire = vint(ty2.field("id").unwrap().number, 1);
    wire.extend(ld(nums.number, &payload));
    let m = ty2.decode(&wire).expect("packed on unpacked field");
    assert_eq!(m.items("nums").unwrap()[0].as_i64(), Some(4));
}

#[test]
fn oneofs_collapse_to_the_last_member() {
    let ty = type_of("oneoftest.desc", "structtest.OneOfTest");
    let reference = fixture("oneoftest.bin");
    let m = ty.decode(&reference).expect("decode oneof fixture");
    assert_eq!(ty.encode(&m).unwrap(), reference);
    let case = m.oneof_case("kind").unwrap();
    assert!(case != 0, "one member is set");
    // Setting two members leaves only the last one.
    let mut m = ty.new_message();
    m.set_i64("num", 3).unwrap();
    m.set_str("text", "later").unwrap();
    assert_eq!(m.oneof_case("kind").unwrap(), ty.field("text").unwrap().number);
    assert!(!m.has("num").unwrap(), "the earlier member is cleared");
    let wire = ty.encode(&m).unwrap();
    assert_eq!(wire, ld(ty.field("text").unwrap().number, b"later"));
    // A decode of both members keeps the last.
    let mut both = vint(ty.field("num").unwrap().number, 5);
    both.extend(ld(ty.field("text").unwrap().number, b"win"));
    let m = ty.decode(&both).expect("decode both members");
    assert_eq!(m.get_str("text").unwrap().as_deref(), Some("win"));
    assert_eq!(m.oneof_case("kind").unwrap(), ty.field("text").unwrap().number);
}

#[test]
fn presence_follows_the_field_rule() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    // An implicit zero on the wire is dropped; an explicit one is kept.
    let wire = vint(1, 0); // i32 = 0, implicit
    let m = ty.decode(&wire).expect("implicit zero");
    assert!(!m.has("i32").unwrap(), "implicit zero stays absent");
    assert_eq!(ty.encode(&m).unwrap(), Vec::<u8>::new());
    let wire = vint(ty.field("opt_i32").unwrap().number, 0); // proto3 optional 0
    let m = ty.decode(&wire).expect("explicit zero");
    assert!(m.has("opt_i32").unwrap(), "explicit zero is present");
    assert_eq!(ty.encode(&m).unwrap(), wire);
    // An implicit empty string on the wire is dropped; an explicit one stays.
    let wire = ld(ty.field("str").unwrap().number, b"");
    let m = ty.decode(&wire).expect("empty implicit string");
    assert!(!m.has("str").unwrap());
    let wire = ld(ty.field("opt_str").unwrap().number, b"");
    let m = ty.decode(&wire).expect("empty explicit string");
    assert!(m.has("opt_str").unwrap());
    assert_eq!(ty.encode(&m).unwrap(), wire);
    // A message field is present even when its body is empty.
    let wire = ld(ty.field("inner").unwrap().number, b"");
    let m = ty.decode(&wire).expect("empty message");
    assert!(m.has("inner").unwrap());
    assert_eq!(ty.encode(&m).unwrap(), wire);
}

#[test]
fn required_fields_fail_the_decode() {
    let ty = type_of("matrix2.desc", "structtest.Scalars2");
    // matrix2.bin carries every required field; drop `id` (field 1) and fail.
    let reference = fixture("matrix2.bin");
    let m = ty.decode(&reference).expect("decode matrix2");
    assert_eq!(ty.encode(&m).unwrap(), reference);
    let mut wire = Vec::new();
    // A minimal body without the required `id`.
    if !m.items("nums").unwrap().is_empty() {
        wire.extend(vint(ty.field("nums").unwrap().number, 1));
    }
    assert_eq!(status_of(ty.decode(&wire)), 6, "missing required field");
    // Encoding the same tree fails the same way.
    let mut empty = ty.new_message();
    empty.set_i64("nums", 1).unwrap();
    assert_eq!(status_of(ty.encode(&empty)), 6);
}

#[test]
fn utf8_is_validated_only_where_the_schema_says() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let wire = ld(14, &[0xed, 0xa0, 0x80]); // a surrogate: invalid UTF-8
    assert_eq!(status_of(ty.decode(&wire)), 1, "proto3 string verifies");
    // proto2 keeps the loose mapping: the bytes stay, a re-encode is exact.
    let ty2 = type_of("matrix2.desc", "structtest.Scalars2");
    let id = ty2.field("id").unwrap().number;
    let str_field = ty2.field("str").unwrap().number;
    let mut wire = vint(id, 1);
    wire.extend(ld(str_field, &[0x80]));
    let m = ty2.decode(&wire).expect("proto2 loose string");
    assert_eq!(m.get_bytes("str").unwrap(), Some(&[0x80u8][..]));
    assert_eq!(ty2.encode(&m).unwrap(), wire);
}

#[test]
fn bad_tags_and_truncation_fail() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    assert_eq!(status_of(ty.decode(&[0x00, 0x00])), 1, "field number 0");
    assert_eq!(status_of(ty.decode(&[0x0e, 0x00])), 1, "wire type 6");
    assert_eq!(status_of(ty.decode(&[0x0f, 0x00])), 1, "wire type 7");
    assert!(ty.decode(&[0x08]).is_err(), "truncated varint");
    assert!(ty.decode(&[0x0a, 0x05, 0x00]).is_err(), "truncated length");
    assert!(ty.decode(&[0x09, 0x00, 0x00, 0x00]).is_err(), "truncated fixed32");
    assert!(ty.decode(&[0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]).is_err(),
            "length past the buffer");
    // A 5-byte tag above UINT32_MAX truncates instead of failing: the official
    // suite expects the two BadTag cases to parse. Here the high bits make tag
    // 0x1000000008, which truncates to tag 8 (field 1, varint).
    let mut wire = vec![0x88, 0x80, 0x80, 0x80, 0x10];
    wire.extend(varint(5));
    let m = ty.decode(&wire).expect("truncating tag");
    assert_eq!(m.get_i64("i32").unwrap(), Some(5));
}

#[test]
fn limits_bound_bytes_and_depth() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let reference = fixture("matrix3.bin");
    let tiny = Limits::new(10, 0);
    assert_eq!(status_of(ty.decode_with(&reference, &tiny)), 5);
    assert_eq!(status_of(Schema::from_bytes(&reference, &tiny).map(|_| ())), 5);
    // A self-nesting message deeper than the cap fails with status 4.
    let node = deep_node();
    let mut wire = Vec::new();
    for _ in 0..(MAX_DEPTH as usize + 1) {
        wire = ld(1, &wire);
    }
    assert_eq!(status_of(node.decode(&wire)), 4, "nesting exceeds the cap");
    let shallow = Limits::new(0, 4);
    let wire = ld(1, &ld(1, &ld(1, &ld(1, &ld(1, b"")))));
    assert_eq!(status_of(node.decode_with(&wire, &shallow)), 4);
    // A depth below the cap still decodes.
    assert!(node.decode_with(&ld(1, b""), &shallow).is_ok());
}

/// `syntax = "proto3"; package deep; message Node { Node kid = 1; }`
fn deep_node() -> MessageType {
    let mut fld = ld(1, b"kid");
    fld.extend(vint(3, 1));
    fld.extend(vint(4, 1));
    fld.extend(vint(5, 11));
    fld.extend(ld(6, b".deep.Node"));
    let mut msg = ld(1, b"Node");
    msg.extend(ld(2, &fld));
    let mut file = ld(1, b"deep.proto");
    file.extend(ld(2, b"deep"));
    file.extend(ld(4, &msg));
    file.extend(ld(12, b"proto3"));
    let set = ld(1, &file);
    let schema = Schema::from_bytes(&set, &Limits::default()).expect("synthetic");
    schema.message("deep.Node").expect("deep.Node")
}

#[test]
fn every_kind_has_a_storage_form() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let expected = [
        ("i32", Kind::Int32),
        ("i64", Kind::Int64),
        ("u32", Kind::Uint32),
        ("u64", Kind::Uint64),
        ("s32", Kind::Sint32),
        ("s64", Kind::Sint64),
        ("fx32", Kind::Fixed32),
        ("fx64", Kind::Fixed64),
        ("sfx32", Kind::Sfixed32),
        ("sfx64", Kind::Sfixed64),
        ("f32", Kind::Float),
        ("f64", Kind::Double),
        ("flag", Kind::Bool),
        ("str", Kind::String),
        ("color", Kind::Enum),
        ("inner", Kind::Message),
    ];
    for (name, kind) in expected {
        assert_eq!(ty.field(name).unwrap().kind, kind, "{name}");
    }
    // A float NaN payload survives a round trip (bits, not a converted value).
    let mut m = ty.new_message();
    let nan = f32::from_bits(0x7fc0_1234);
    m.set_f32("f32", nan).unwrap();
    let back = ty.decode(&ty.encode(&m).unwrap()).unwrap();
    let bits = match back.get("f32").unwrap() {
        Some(Value::F32(bits)) => *bits,
        other => panic!("f32 slot is {other:?}"),
    };
    assert_eq!(bits, 0x7fc0_1234);
    let _ = CoreError::new("");
    let _ = Presence::Implicit;
    let _ = load("matrix3.desc");
}
