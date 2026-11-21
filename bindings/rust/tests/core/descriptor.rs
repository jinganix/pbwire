//! Stage 1 gate for the pure-Rust core: the descriptor reader over the same
//! `FileDescriptorSet` subset the C core reads (`src/descriptor_set.c`).
//!
//! Only facts the codec depends on: field count and order, the four presence
//! kinds, repetition and packedness, kind mapping, oneofs, groups and the
//! transitive required closure. Every assertion comes from a `.desc` fixture
//! the C tests already pin, so the two readers cannot drift.

use std::path::PathBuf;

use pbwire::core::{Field, Kind, MessageType, Presence, Schema};

fn data_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../c/tests/data")
}

fn load(file: &str) -> Schema {
    let path = data_dir().join(file);
    Schema::from_file(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn type_of(file: &str, name: &str) -> MessageType {
    load(file).message(name).unwrap_or_else(|e| panic!("{file}: {e}"))
}

fn field<'a>(ty: &'a MessageType, name: &str) -> &'a Field {
    ty.field(name).unwrap_or_else(|e| panic!("{}: {e}", ty.name()))
}

fn type_name(ty: &MessageType, field_name: &str) -> String {
    let f = field(ty, field_name);
    let idx = f.msg_type.expect("message field without a type");
    // The schema outlives the handle through the same `Rc`; reach it via the
    // field's own def through a fresh lookup.
    let schema = ty.schema();
    schema.messages[idx].full_name.clone()
}

/// The descriptor test surface: 22 fields of `structtest.Scalars3`, the four
/// presence kinds, proto2 requiredness, groups and oneofs.
#[test]
fn descriptor() {
    // ---------------------------------------------------------- matrix3
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let def = ty.def();
    assert_eq!(def.full_name, "structtest.Scalars3");
    assert_eq!(def.fields.len(), 22, "matrix3.desc: 22 fields");
    for (i, f) in def.fields.iter().enumerate() {
        assert_eq!(f.number, i as u32 + 1, "field {} is numbered {}", i + 1, f.number);
    }
    assert_eq!(def.fields[0].name, "i32", "first field is i32");
    assert_eq!(def.fields[21].name, "flags", "last field is flags");

    // proto3 plain scalars have no has-bit...
    assert_eq!(field(&ty, "i32").presence, Presence::Implicit);
    assert_eq!(field(&ty, "str").presence, Presence::Implicit);
    // ...while `optional` in a proto3 file is explicit presence.
    assert_eq!(field(&ty, "opt_i32").presence, Presence::Explicit);
    assert_eq!(field(&ty, "opt_str").presence, Presence::Explicit);
    // ...and a message field always has it.
    assert_eq!(field(&ty, "inner").presence, Presence::MessagePresence);

    let flags = field(&ty, "flags");
    assert!(flags.repeated, "flags repeated");
    assert_eq!(flags.kind, Kind::Bool, "flags is bool");
    assert!(flags.packed, "proto3 repeated numerics default to packed");
    assert_eq!(flags.wt, 2, "a packed field encodes under the LEN tag");

    let nums = field(&ty, "nums");
    assert!(nums.repeated, "nums repeated");
    assert_eq!(nums.kind, Kind::Int32, "nums is int32");
    assert!(nums.packed, "nums packed");
    assert_eq!(nums.elem_wt, 0, "the unpacked element form is still accepted");

    assert_eq!(field(&ty, "i64").kind, Kind::Int64);
    assert_eq!(field(&ty, "u64").kind, Kind::Uint64);
    assert_eq!(field(&ty, "s64").kind, Kind::Sint64);
    assert_eq!(field(&ty, "fx64").kind, Kind::Fixed64);
    assert_eq!(field(&ty, "sfx64").kind, Kind::Sfixed64);
    assert_eq!(field(&ty, "f32").kind, Kind::Float);
    assert_eq!(field(&ty, "f64").kind, Kind::Double);
    assert_eq!(field(&ty, "color").kind, Kind::Enum);
    assert_eq!(field(&ty, "inner").kind, Kind::Message);
    assert_eq!(type_name(&ty, "inner"), "structtest.Inner3", "inner type");
    // proto3 strings verify UTF-8 on parse.
    assert!(field(&ty, "str").utf8, "proto3 str verifies utf8");
    // The enum is open (proto3), and its values are indexed by name.
    let color = &ty.schema().enums[field(&ty, "color").enum_type.unwrap()];
    assert!(!color.closed, "proto3 enum is open");
    assert_eq!(color.by_name["COLOR3_GREEN"], 2);
    assert_eq!(color.by_number[&2], "COLOR3_GREEN");

    // ---------------------------------------------------------- matrix2
    let ty2 = type_of("matrix2.desc", "structtest.Scalars2");
    assert_eq!(field(&ty2, "id").presence, Presence::Required, "proto2 required");
    assert!(ty2.def().has_required, "Scalars2 has a required field");
    // Inner2.name is required, so Scalars2 is required transitively too.
    assert!(
        type_of("matrix2.desc", "structtest.Inner2").def().has_required,
        "Inner2 has a required field"
    );
    assert!(ty2.def().deep_required, "Scalars2 is transitively required");

    // proto2 packs only with [packed = true].
    let p2nums = field(&ty2, "nums");
    assert!(p2nums.repeated && !p2nums.packed, "proto2 nums is unpacked");
    assert!(field(&ty2, "packed_nums").packed, "proto2 packed_nums is packed");
    // proto2 strings do not verify UTF-8.
    assert!(!field(&ty2, "str").utf8, "proto2 str does not verify utf8");

    // ------------------------------------------------------- grouptest
    let g = type_of("grouptest.desc", "structtest.GroupTest");
    let ginner = field(&g, "inner");
    assert_eq!(ginner.kind, Kind::Group, "inner is a group");
    assert_eq!(ginner.wt, 3, "start-group tag");
    assert_eq!(type_name(&g, "inner"), "structtest.GroupTest.Inner", "group type");
    assert_eq!(ginner.tag.as_slice(), &[0x0b], "start tag 0x0b");
    assert_eq!(ginner.tag_end.as_slice(), &[0x0c], "end tag 0x0c");
    let gitem = field(&g, "item");
    assert_eq!(gitem.kind, Kind::Group, "item is a group");
    assert!(gitem.repeated, "item is repeated");

    // ------------------------------------------------------- oneoftest
    let o = type_of("oneoftest.desc", "structtest.OneOfTest");
    assert_eq!(o.def().oneof_count, 1, "one oneof");
    assert_eq!(o.def().oneof_by_name["kind"], 0, "oneof kind is slot 0");
    for name in ["num", "text", "sub", "blob"] {
        let f = field(&o, name);
        assert_eq!(f.oneof, 0, "{name} is a member of the oneof");
        assert_eq!(f.presence, Presence::Explicit, "{name} has explicit presence");
    }
    assert_eq!(field(&o, "id").oneof, -1, "id is not in the oneof");

    // ------------------------------------------------------- synthetic
    let node = deep_type();
    assert_eq!(node.def().fields.len(), 1, "synthetic: one field");
    let kid = &node.def().fields[0];
    assert_eq!(
        kid.msg_type,
        Some(0),
        "the field points back at Node (index 0)"
    );
    assert_eq!(
        node.schema().messages[kid.msg_type.unwrap()].full_name,
        "deep.Node"
    );
    assert!(!kid.utf8, "a message field carries no utf8 flag");
    assert!(!node.def().has_required, "proto3 message has no required field");
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
    let mut out = vec![(fnum * 8 + 2) as u8];
    out.extend(varint(payload.len() as u64));
    out.extend_from_slice(payload);
    out
}

fn vint(fnum: u32, v: u64) -> Vec<u8> {
    let mut out = vec![(fnum * 8) as u8];
    out.extend(varint(v));
    out
}

/// A hand-built `FileDescriptorSet` for a self-nesting proto3 message:
/// `syntax = "proto3"; package deep; message Node { Node kid = 1; }`.
fn deep_type() -> MessageType {
    let mut fld = ld(1, b"kid");
    fld.extend(vint(3, 1)); // number
    fld.extend(vint(4, 1)); // label optional
    fld.extend(vint(5, 11)); // type message
    fld.extend(ld(6, b".deep.Node"));
    let mut msg = ld(1, b"Node");
    msg.extend(ld(2, &fld));
    let mut file = ld(1, b"deep.proto");
    file.extend(ld(2, b"deep"));
    file.extend(ld(4, &msg));
    file.extend(ld(12, b"proto3"));
    let set = ld(1, &file);
    let schema = Schema::from_bytes(&set, &pbwire::core::Limits::default()).expect("synthetic");
    schema.message("deep.Node").expect("deep.Node")
}
