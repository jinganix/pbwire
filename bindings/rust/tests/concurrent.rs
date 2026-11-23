//! Concurrent-read stress over the matrix3 oracle: 8 threads x 200 iters.
//!
//! * `test_concurrent_encode_shared`: serial encode-once == matrix3.bin, then
//!   each thread does (a) a per-thread message encode == want and (b) a
//!   shared-read-only-message encode == want.
//! * `test_concurrent_decode_shared`: 8x200 fresh-target decodes + field
//!   checks, no re-encode.
//!
//! Throughput goes to eprintln only (never asserted).

use pbwire::{Message, Schema, Type};
use std::time::Instant;

const DESC: &str = "../c/tests/data/matrix3.desc";
const ORACLE: &[u8] = include_bytes!("../../c/tests/data/matrix3.bin");
const MSG: &str = "structtest.Scalars3";
const INNER_MSG: &str = "structtest.Inner3";

fn fill<'a>(ty: &'a Type<'a>, inner_ty: &'a Type<'a>) -> Message<'a> {
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

// Test-local raw-pointer wrappers so `std::thread::scope` closures are
// `Send`. Sound here because: the schema is immutable after load, encode only
// reads the message tree (C core concurrent-read safety proven by Tasks 1-2),
// the scope joins every thread before the pointed-to schema/message drops, and
// decode uses per-thread target messages (only the schema/descriptor is
// shared). No library change: `Schema`/`Message` stay !Send+!Sync.
#[derive(Copy, Clone)]
struct SharedSchema(*const Schema);
// SAFETY: schema immutable after load; see comment above.
unsafe impl Send for SharedSchema {}
// SAFETY: schema immutable after load; see comment above.
unsafe impl Sync for SharedSchema {}

impl SharedSchema {
    // By-value accessor: a `move` closure calling `ptr()` captures the whole
    // `Send` wrapper instead of the `*const` field (disjoint capture).
    fn ptr(self) -> *const Schema {
        self.0
    }
}

#[derive(Copy, Clone)]
struct SharedMsg<'a>(*const Message<'a>);
// SAFETY: encode only reads the tree; scope-joined before drop; see above.
unsafe impl Send for SharedMsg<'_> {}
// SAFETY: encode only reads the tree; scope-joined before drop; see above.
unsafe impl Sync for SharedMsg<'_> {}

impl<'a> SharedMsg<'a> {
    // By-value accessor: see `SharedSchema::ptr`.
    fn ptr(self) -> *const Message<'a> {
        self.0
    }
}

fn check_decoded(ty: &Type<'_>, inner_ty: &Type<'_>, decoded: &Message<'_>) {
    assert_eq!(decoded.get_i64(ty.field("i32").unwrap()), i32::MIN as i64);
    assert!(decoded.get_bool(ty.field("flag").unwrap()));
    assert_eq!(decoded.get_str(ty.field("str").unwrap()), Some("hello"));
    assert_eq!(decoded.get_f64(ty.field("f64").unwrap()), -123456.789);
    assert_eq!(decoded.get_i64(ty.field("color").unwrap()), 2);
    let inner = decoded.get_message(ty.field("inner").unwrap()).expect("inner");
    assert_eq!(inner.get_str(inner_ty.field("name").unwrap()), Some("kid"));
    assert_eq!(inner.get_i64(inner_ty.field("value").unwrap()), 42);
    let nums = decoded.value(ty.field("nums").unwrap()).expect("nums");
    assert_eq!(nums.len(), 3);
}

#[test]
fn test_concurrent_encode_shared() {
    // Serial encode-once baseline: must equal the protoc oracle.
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let ty = schema.message(MSG).expect("message type");
    let inner_ty = schema.message(INNER_MSG).expect("inner type");
    let msg = fill(&ty, &inner_ty);
    let want = msg.encode().expect("encode");
    assert_eq!(want, ORACLE, "encoded bytes differ from protoc");

    let shared_schema = SharedSchema(&schema as *const Schema);
    let shared_msg = SharedMsg(&msg as *const Message<'_>);
    let want_ref = &want;
    let start = Instant::now();
    std::thread::scope(|s| {
        for _ in 0..8 {
            // Copy the wrappers into the `move` closure so it owns a
            // `Send` handle instead of borrowing the raw-pointer field.
            let sh_schema = shared_schema;
            let sh_msg = shared_msg;
            s.spawn(move || {
                // Per-thread descriptor lookup off the shared (immutable) schema.
                let schema: &Schema = unsafe { &*sh_schema.ptr() };
                let ty = schema.message(MSG).expect("message type");
                let inner_ty = schema.message(INNER_MSG).expect("inner type");
                for _ in 0..200 {
                    // (a) per-thread message: build fresh, encode, compare.
                    let m = fill(&ty, &inner_ty);
                    assert_eq!(m.encode().expect("encode"), *want_ref);
                    // (b) shared read-only message: encode the shared tree.
                    let shared: &Message<'_> = unsafe { &*sh_msg.ptr() };
                    assert_eq!(shared.encode().expect("encode"), *want_ref);
                }
            });
        }
    });
    let total = 8 * 200 * 2;
    eprintln!(
        "test_concurrent_encode_shared: {total} encodes in {:?} ({:.0}/s)",
        start.elapsed(),
        total as f64 / start.elapsed().as_secs_f64()
    );
}

#[test]
fn test_concurrent_decode_shared() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let shared_schema = SharedSchema(&schema as *const Schema);
    let start = Instant::now();
    std::thread::scope(|s| {
        for _ in 0..8 {
            let sh_schema = shared_schema;
            s.spawn(move || {
                let schema: &Schema = unsafe { &*sh_schema.ptr() };
                let ty = schema.message(MSG).expect("message type");
                let inner_ty = schema.message(INNER_MSG).expect("inner type");
                for _ in 0..200 {
                    // Fresh per-thread target: only the schema is shared.
                    // No re-encode here: this test covers the decode path only.
                    let decoded = ty.decode(ORACLE).expect("decode");
                    check_decoded(&ty, &inner_ty, &decoded);
                }
            });
        }
    });
    let total = 8 * 200;
    eprintln!(
        "test_concurrent_decode_shared: {total} decodes in {:?} ({:.0}/s)",
        start.elapsed(),
        total as f64 / start.elapsed().as_secs_f64()
    );
}
