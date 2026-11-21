/* Rust binding benchmark against rust-protobuf's generated code and prost
 * (the fastest widely used Rust runtime) on the same matrix3.proto and the
 * four standard tiers from BENCHMARKS.md.
 *
 * The tree row times the Go-parity full task: a fresh native Message per
 * iteration (one set_* per field), encode_to into the reused buffer, drop;
 * decode, one get_* per field into a sink the optimizer cannot drop, drop.
 * Message frees itself on drop, so allocation and free both sit inside the
 * timer. The proxy row keeps the old prebuilt shape (a held handle's
 * encode_to, one-shot decode-and-drop with no materialization), relabelled
 * not re-measured, never the headline.
 *
 * The same payload as the C/Go/Java rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
 * sides must produce identical bytes before anything is timed. Encode reuses
 * one buffer per implementation (pbwire encode_to(), rust-protobuf
 * write_to_vec(), prost encode()): each side's idiomatic hot loop, so the
 * steady state allocates nothing anywhere and the codec is timed, not the
 * allocator. A one-shot pbwire encode() would pay the core malloc plus a
 * 75 KB copy that neither baseline pays; encode_to() exists so hot loops do
 * not. Baseline decode stays one-shot on every side (parse_from_bytes /
 * decode / Type::decode, each with its own arena); the tree row instead
 * decodes, reads every field back out, then drops the message.
 *
 *   ./bench.sh          generate, build, run
 */

#[path = "../gen/matrix3.rs"]
mod matrix3;
/* The pbwire-side model (the tree and the flat mirror) is the same generated
 * file the crate's tests use; regenerating it is `make -C codegen gen`. */
#[path = "../../tests/gen/matrix3_pbwire.rs"]
mod models;

/* The third arm: the pure-Rust core (`cargo` feature `core`), one file so the
 * arm has no crate of its own. */
#[path = "../../pbench/core_arm.rs"]
mod core_arm;

mod prost_gen {
    include!(concat!(env!("OUT_DIR"), "/structtest.rs"));
}

use std::ffi::{c_char, CString};
use std::time::Instant;

use matrix3::{Color3, Inner3, Scalars3};
use models::{Inner3Flat, Scalars3Flat};
use pbwire::{Field, FlatImage, Message, Schema, Type};
use prost::Message as _;
use protobuf::{EnumOrUnknown, Message as _};

const CASES: [&str; 4] = ["small", "medium", "large", "xlarge"];
const REPEATS: [usize; 4] = [0, 1, 100, 3000];
const WANT: [usize; 4] = [136, 174, 2469, 74913];
const ITERATIONS: [usize; 4] = [50_000, 50_000, 5_000, 500];
const PASSES: usize = 3;

static mut FAILURES: usize = 0;

fn main() {
    let path = match std::env::args().nth(1) {
        Some(path) => path,
        None => {
            eprintln!("usage: bench <descriptor-set>");
            std::process::exit(2);
        }
    };
    let schema = Schema::from_file(&path).unwrap_or_else(|e| {
        eprintln!("load {path}: {e}");
        std::process::exit(1);
    });
    let ty = schema.message("structtest.Scalars3").expect("message type");
    let inner_ty = schema.message("structtest.Inner3").expect("inner type");
    let core = core_arm::CoreArm::load(&path).unwrap_or_else(|e| {
        eprintln!("core arm: {e}");
        std::process::exit(1);
    });

    for tier in 0..CASES.len() {
        run_case(&schema, &ty, &inner_ty, &core, tier);
    }
    if unsafe { FAILURES } != 0 {
        eprintln!("{} check(s) failed", unsafe { FAILURES });
        std::process::exit(1);
    }
}

fn run_case(schema: &Schema, ty: &Type<'_>, inner_ty: &Type<'_>, core: &core_arm::CoreArm, tier: usize) {
    let label = CASES[tier];
    let n = REPEATS[tier];
    let iterations = ITERATIONS[tier];

    let reference = fill_official(n).write_to_bytes().expect("official encode");
    if reference.len() != WANT[tier] {
        fail(
            label,
            &format!(
                "official encoder wrote {} bytes, expected {}",
                reference.len(),
                WANT[tier]
            ),
        );
        return;
    }
    let message = fill_pbwire(ty, inner_ty, n);
    let got = message.encode().expect("encode");
    if got != reference {
        fail(
            label,
            &format!(
                "pbwire-rust wrote {} bytes, protobuf-rust {}",
                got.len(),
                reference.len()
            ),
        );
        return;
    }
    println!(
        "OK   {label} encoding parity, x{n} ({} bytes)",
        reference.len()
    );

    // The in-place arm: the generated `#[repr(C)]` mirror is the native
    // object, one call per op. Parity first, exactly like the tree.
    let mut flat = schema.in_place::<Scalars3Flat>().expect("flat plan");
    let payload = FlatPayload::new(n);
    let flat_image = flat.encode_in_place(&payload.image).expect("flat encode");
    if flat_image != reference {
        fail(
            label,
            &format!(
                "pbwire-rust-shim wrote {} bytes, protobuf-rust {}",
                flat_image.len(),
                reference.len()
            ),
        );
        return;
    }
    println!("OK   {label} flat-image parity, x{n}");

    // The core arm: the pure-Rust codec, same payload, same reference bytes.
    if let Err(what) = core.gate(&reference, n) {
        fail(label, &what);
        return;
    }
    println!("OK   {label} pure-core parity, x{n}");

    // Decode must round-trip byte-exactly and expose the same values.
    let back = ty.decode(&reference).expect("decode");
    let back_ok = back.encode().expect("encode") == reference
        && back.get_str(ty.field("str").expect("field")) == Some("hello")
        && back
            .get_message(ty.field("inner").expect("field"))
            .as_ref()
            .and_then(|m| m.get_str(inner_ty.field("name").expect("field")))
            == Some("kid")
        && back
            .value(ty.field("nums").expect("field"))
            .map_or(n == 0, |v| v.len() == n)
        && (n == 0
            || back
                .value(ty.field("nums").expect("field"))
                .and_then(|v| v.at(n - 1))
                .map_or(false, |v| v.as_i64() == n as i64 - 4));
    if !back_ok {
        fail(label, "pbwire-rust decode differs");
        return;
    }

    // Go-parity gate on the tree full task: a fresh native build encodes to
    // the reference bytes, a fresh decode exposes every gate-checked field,
    // and the decoded tree re-encodes byte-exactly. The timer consumes the
    // same field list (TreeFields), so gate and timer agree by construction.
    let fields = TreeFields::resolve(ty, inner_ty);
    let fresh = fill_pbwire(ty, inner_ty, n);
    match fresh.encode() {
        Ok(bytes) if bytes == reference => {}
        Ok(bytes) => {
            fail(
                label,
                &format!(
                    "fresh tree wrote {} bytes, expected {}",
                    bytes.len(),
                    reference.len()
                ),
            );
            return;
        }
        Err(e) => {
            fail(label, &format!("fresh tree encode failed: {e}"));
            return;
        }
    }
    drop(fresh);
    match ty.decode(&reference) {
        Ok(back) => {
            if !tree_fields_ok(&back, &fields, n) {
                fail(label, "fresh tree decode field compare differs");
                return;
            }
            if back.encode().expect("encode") != reference {
                fail(label, "re-encode of the decoded tree differs");
                return;
            }
            println!("OK   {label} tree full-task parity, x{n}");
            drop(back);
        }
        Err(e) => {
            fail(label, &format!("fresh tree decode failed: {e}"));
            return;
        }
    }

    let official = fill_official(n);
    let prost_msg = fill_prost(n);
    if prost_msg.encode_to_vec() != reference {
        fail(label, "prost wrote different bytes than protobuf-rust");
        return;
    }
    // Hot-loop shape on every side: one reused buffer, cleared per op.
    let mut pbwire_buf = Vec::new();
    let mut official_buf = Vec::new();
    let mut prost_buf = Vec::new();
    // Tier line shows the reference op's count; slower ops keep the floor.
    // (The fast proxy ops calibrate from the same floor, so they never
    // inflate the tree rows.)
    let tier_count = calibrate(
        &mut || {
            pbwire_buf.clear();
            message
                .encode_to(&mut pbwire_buf)
                .expect("encode")
                .len()
        },
        iterations,
    );
    // The tree full task (Go parity): a fresh native build per iteration,
    // encoded, dropped; a fresh decode per iteration, read back per field,
    // dropped. The encode buffer is reused exactly as the header says;
    // everything else is allocated and freed (dropped) inside the timer.
    let tree_encode = measure(iterations, || {
        pbwire_buf.clear();
        let m = fill_pbwire(ty, inner_ty, n);
        let len = m.encode_to(&mut pbwire_buf).expect("encode").len();
        drop(m);
        len
    });
    let tree_decode = measure(iterations, || {
        let back = ty.decode(&reference).expect("decode");
        let sink = consume_tree(&back, &fields, n);
        drop(back);
        sink
    });
    // The proxy task: the old prebuilt shape, relabelled not re-measured as
    // a new task. A held handle re-encoded, decoded-and-dropped with no
    // materialization.
    let proxy_encode = measure(iterations, || {
        pbwire_buf.clear();
        message.encode_to(&mut pbwire_buf).expect("encode").len()
    });
    let proxy_decode = measure(iterations, || ty.decode(&reference).expect("decode"));
    let mut flat_image = FlatImage::<Scalars3Flat>::new();
    flat.decode_in_place(&reference, &mut flat_image)
        .expect("flat decode");
    if flat_image.value.str.is_null() || flat_image.value.nums_count != n {
        fail(label, "pbwire-rust-shim decode differs");
        return;
    }
    let flat_encode = measure(iterations, || {
        flat.encode_in_place(&payload.image).expect("flat encode").len()
    });
    let flat_decode = measure(iterations, || {
        flat.decode_in_place(&reference, &mut flat_image)
            .expect("flat decode")
    });
    let official_encode = measure(iterations, || {
        official_buf.clear();
        official.write_to_vec(&mut official_buf).expect("encode");
        official_buf.len()
    });
    let official_decode = measure(iterations, || {
        Scalars3::parse_from_bytes(&reference).expect("decode")
    });
    let prost_encode = measure(iterations, || {
        prost_buf.clear();
        prost_msg.encode(&mut prost_buf).expect("encode");
        prost_buf.len()
    });
    let prost_decode = measure(iterations, || {
        prost_gen::Scalars3::decode(&reference[..]).expect("decode")
    });
    let core_message = core.build(n);
    let mut core_buf = Vec::new();
    let core_encode = measure(iterations, || core.encode_into(&core_message, &mut core_buf));
    let core_decode = measure(iterations, || core.decode(&reference).expect("core decode"));

    println!(
        "{label}: {} bytes, {tier_count} iterations (repeat field x{n})",
        reference.len()
    );
    println!("  {:<12} {:<7} {:>12} {:>10}", "impl", "op", "ns/op", "MB/s");
    row("pbwire-rust-tree", "encode", reference.len(), tree_encode);
    row("pbwire-rust-shim", "encode", reference.len(), flat_encode);
    row("pbwire-rust-core", "encode", reference.len(), core_encode);
    row("pbwire-rust-proxy", "encode", reference.len(), proxy_encode);
    row("protobuf-rust", "encode", reference.len(), official_encode);
    row("prost", "encode", reference.len(), prost_encode);
    row("pbwire-rust-tree", "decode", reference.len(), tree_decode);
    row("pbwire-rust-shim", "decode", reference.len(), flat_decode);
    row("pbwire-rust-core", "decode", reference.len(), core_decode);
    row("pbwire-rust-proxy", "decode", reference.len(), proxy_decode);
    row("protobuf-rust", "decode", reference.len(), official_decode);
    row("prost", "decode", reference.len(), prost_decode);
    println!();
}

fn fill_official(n: usize) -> Scalars3 {
    let mut m = Scalars3::new();
    m.i32 = i32::MIN;
    m.i64 = i64::MAX;
    m.u32 = u32::MAX;
    m.u64 = u64::MAX;
    m.s32 = i32::MIN;
    m.s64 = i64::MIN;
    m.fx32 = 0xdeadbeef;
    m.fx64 = 72623859790382856;
    m.sfx32 = i32::MIN;
    m.sfx64 = i64::MIN;
    m.f32 = f32::INFINITY;
    m.f64 = -123456.789;
    m.flag = true;
    m.str = "hello".to_string();
    m.color = EnumOrUnknown::new(Color3::COLOR3_GREEN);
    m.inner = protobuf::MessageField::some(Inner3 {
        name: "kid".to_string(),
        value: 42,
        ..Default::default()
    });
    m.opt_i32 = Some(-7);
    m.opt_str = Some("opt".to_string());
    for i in 0..n {
        m.nums.push(i as i32 - 3);
        m.tags.push("tag".to_string());
        m.inners.push(Inner3 {
            name: "grandkid".to_string(),
            value: i as i32 + 1,
            ..Default::default()
        });
        m.flags.push(i % 2 == 0);
    }
    m
}

fn fill_prost(n: usize) -> prost_gen::Scalars3 {
    let mut m = prost_gen::Scalars3 {
        i32: i32::MIN,
        i64: i64::MAX,
        u32: u32::MAX,
        u64: u64::MAX,
        s32: i32::MIN,
        s64: i64::MIN,
        fx32: 0xdeadbeef,
        fx64: 72623859790382856,
        sfx32: i32::MIN,
        sfx64: i64::MIN,
        f32: f32::INFINITY,
        f64: -123456.789,
        flag: true,
        str: "hello".to_string(),
        color: prost_gen::Color3::Green as i32,
        inner: Some(prost_gen::Inner3 {
            name: "kid".to_string(),
            value: 42,
        }),
        opt_i32: Some(-7),
        opt_str: Some("opt".to_string()),
        ..Default::default()
    };
    for i in 0..n {
        m.nums.push(i as i32 - 3);
        m.tags.push("tag".to_string());
        m.inners.push(prost_gen::Inner3 {
            name: "grandkid".to_string(),
            value: i as i32 + 1,
        });
        m.flags.push(i % 2 == 0);
    }
    m
}

/// Field handles resolved once per tier so the tree timer pays only the
/// per-field get, never the name lookup. The gate (`tree_fields_ok`) and the
/// timer (`consume_tree`) share this list by construction.
struct TreeFields<'s> {
    i32_f: Field<'s>,
    i64_f: Field<'s>,
    u32_f: Field<'s>,
    u64_f: Field<'s>,
    s32_f: Field<'s>,
    s64_f: Field<'s>,
    fx32_f: Field<'s>,
    fx64_f: Field<'s>,
    sfx32_f: Field<'s>,
    sfx64_f: Field<'s>,
    f32_f: Field<'s>,
    f64_f: Field<'s>,
    flag: Field<'s>,
    str_f: Field<'s>,
    color: Field<'s>,
    inner: Field<'s>,
    opt_i32: Field<'s>,
    opt_str: Field<'s>,
    nums: Field<'s>,
    tags: Field<'s>,
    inners: Field<'s>,
    flags: Field<'s>,
    inner_name: Field<'s>,
    inner_value: Field<'s>,
}

impl<'s> TreeFields<'s> {
    fn resolve<'t, 'a>(ty: &'t Type<'a>, inner_ty: &'t Type<'a>) -> TreeFields<'t> {
        TreeFields {
            i32_f: ty.field("i32").expect("field"),
            i64_f: ty.field("i64").expect("field"),
            u32_f: ty.field("u32").expect("field"),
            u64_f: ty.field("u64").expect("field"),
            s32_f: ty.field("s32").expect("field"),
            s64_f: ty.field("s64").expect("field"),
            fx32_f: ty.field("fx32").expect("field"),
            fx64_f: ty.field("fx64").expect("field"),
            sfx32_f: ty.field("sfx32").expect("field"),
            sfx64_f: ty.field("sfx64").expect("field"),
            f32_f: ty.field("f32").expect("field"),
            f64_f: ty.field("f64").expect("field"),
            flag: ty.field("flag").expect("field"),
            str_f: ty.field("str").expect("field"),
            color: ty.field("color").expect("field"),
            inner: ty.field("inner").expect("field"),
            opt_i32: ty.field("opt_i32").expect("field"),
            opt_str: ty.field("opt_str").expect("field"),
            nums: ty.field("nums").expect("field"),
            tags: ty.field("tags").expect("field"),
            inners: ty.field("inners").expect("field"),
            flags: ty.field("flags").expect("field"),
            inner_name: inner_ty.field("name").expect("field"),
            inner_value: inner_ty.field("value").expect("field"),
        }
    }
}

/// Gate half of the shared field list: every field the fixture sets must read
/// back exactly. Repeated fields contribute their length plus the first and
/// last elements.
fn tree_fields_ok(m: &Message, f: &TreeFields, n: usize) -> bool {
    if m.get_i64(f.i32_f) != i32::MIN as i64
        || m.get_i64(f.i64_f) != i64::MAX
        || m.get_i64(f.u32_f) != u32::MAX as i64
        || m.get_i64(f.u64_f) != -1
        || m.get_i64(f.s32_f) != i32::MIN as i64
        || m.get_i64(f.s64_f) != i64::MIN
        || m.get_i64(f.fx32_f) != 0xdeadbeef
        || m.get_i64(f.fx64_f) != 72623859790382856
        || m.get_i64(f.sfx32_f) != i32::MIN as i64
        || m.get_i64(f.sfx64_f) != i64::MIN
        || m.get_i64(f.color) != Color3::COLOR3_GREEN as i64
        || m.get_f32(f.f32_f) != f32::INFINITY
        || m.get_f64(f.f64_f) != -123456.789
        || !m.get_bool(f.flag)
        || m.get_str(f.str_f) != Some("hello")
        || !m.has(f.opt_i32)
        || m.get_i64(f.opt_i32) != -7
        || m.get_str(f.opt_str) != Some("opt")
    {
        return false;
    }
    match m.get_message(f.inner) {
        Some(inner) => {
            if inner.get_str(f.inner_name) != Some("kid")
                || inner.get_i64(f.inner_value) != 42
            {
                return false;
            }
        }
        None => return false,
    }
    match m.value(f.nums) {
        Some(v) if v.len() == n => {
            if n > 0
                && (!v.at(0).map_or(false, |e| e.as_i64() == -3)
                    || !v.at(n - 1).map_or(false, |e| e.as_i64() == n as i64 - 4))
            {
                return false;
            }
        }
        None if n == 0 => {}
        _ => return false,
    }
    match m.value(f.tags) {
        Some(v) if v.len() == n => {
            if n > 0 && !v.at(0).map_or(false, |e| e.as_str() == Some("tag")) {
                return false;
            }
        }
        None if n == 0 => {}
        _ => return false,
    }
    match m.value(f.inners) {
        Some(v) if v.len() == n => {
            if n > 0 {
                let first_ok = v.at(0).and_then(|e| e.as_message()).map_or(false, |im| {
                    im.get_str(f.inner_name) == Some("grandkid")
                        && im.get_i64(f.inner_value) == 1
                });
                let last_ok = v
                    .at(n - 1)
                    .and_then(|e| e.as_message())
                    .map_or(false, |im| im.get_i64(f.inner_value) == n as i64);
                if !first_ok || !last_ok {
                    return false;
                }
            }
        }
        None if n == 0 => {}
        _ => return false,
    }
    match m.value(f.flags) {
        Some(v) if v.len() == n => {
            if n > 0
                && (!v.at(0).map_or(false, |e| e.as_bool())
                    || !v
                        .at(n - 1)
                        .map_or(false, |e| e.as_bool() == ((n - 1) % 2 == 0)))
            {
                return false;
            }
        }
        None if n == 0 => {}
        _ => return false,
    }
    true
}

/// Timer half of the shared field list: folds every gate-checked field into
/// a sink the optimizer cannot drop. Same reads as `tree_fields_ok`.
fn consume_tree(m: &Message, f: &TreeFields, n: usize) -> u64 {
    let mut acc: u64 = 0;
    acc = acc.wrapping_add(m.get_i64(f.i32_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.i64_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.u32_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.u64_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.s32_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.s64_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.fx32_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.fx64_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.sfx32_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.sfx64_f) as u64);
    acc = acc.wrapping_add(m.get_i64(f.color) as u64);
    acc = acc.wrapping_add(m.get_f32(f.f32_f).to_bits() as u64);
    acc = acc.wrapping_add(m.get_f64(f.f64_f).to_bits());
    acc ^= m.get_bool(f.flag) as u64;
    acc ^= m.get_str(f.str_f).map_or(0, |s| s.len() as u64);
    acc = acc.wrapping_add(m.get_i64(f.opt_i32) as u64);
    acc ^= m.get_str(f.opt_str).map_or(0, |s| s.len() as u64);
    if let Some(inner) = m.get_message(f.inner) {
        acc ^= inner.get_str(f.inner_name).map_or(0, |s| s.len() as u64);
        acc = acc.wrapping_add(inner.get_i64(f.inner_value) as u64);
    }
    if let Some(v) = m.value(f.nums) {
        acc ^= v.len() as u64;
        if n > 0 {
            acc = acc.wrapping_add(v.at(0).map_or(0, |e| e.as_i64() as u64));
            acc = acc.wrapping_add(v.at(n - 1).map_or(0, |e| e.as_i64() as u64));
        }
    }
    if let Some(v) = m.value(f.tags) {
        acc ^= v.len() as u64;
        if n > 0 {
            acc ^= v.at(0).map_or(0, |e| e.as_str().map_or(0, |s| s.len() as u64));
        }
    }
    if let Some(v) = m.value(f.inners) {
        acc ^= v.len() as u64;
        if n > 0 {
            acc = acc.wrapping_add(
                v.at(0)
                    .and_then(|e| e.as_message())
                    .map_or(0, |im| im.get_i64(f.inner_value) as u64),
            );
            acc = acc.wrapping_add(
                v.at(n - 1)
                    .and_then(|e| e.as_message())
                    .map_or(0, |im| im.get_i64(f.inner_value) as u64),
            );
        }
    }
    if let Some(v) = m.value(f.flags) {
        acc ^= v.len() as u64;
        if n > 0 {
            acc ^= v.at(0).map_or(0, |e| e.as_bool() as u64);
            acc ^= v.at(n - 1).map_or(0, |e| e.as_bool() as u64);
        }
    }
    std::hint::black_box(acc)
}

fn fill_pbwire<'a>(ty: &'a Type<'a>, inner_ty: &'a Type<'a>, n: usize) -> Message<'a> {
    let mut m = ty.new_message();
    m.set_i64(ty.field("i32").unwrap(), i32::MIN as i64)
        .set_i64(ty.field("i64").unwrap(), i64::MAX)
        .set_i64(ty.field("u32").unwrap(), u32::MAX as i64)
        .set_i64(ty.field("u64").unwrap(), -1)
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
        .set_i64(ty.field("color").unwrap(), Color3::COLOR3_GREEN as i64)
        .set_i64(ty.field("opt_i32").unwrap(), -7)
        .set_str(ty.field("opt_str").unwrap(), "opt");
    m.set_message(ty.field("inner").unwrap())
        .set_str(inner_ty.field("name").unwrap(), "kid")
        .set_i64(inner_ty.field("value").unwrap(), 42);
    for i in 0..n {
        m.set_i64(ty.field("nums").unwrap(), i as i64 - 3)
            .set_str(ty.field("tags").unwrap(), "tag")
            .set_bool(ty.field("flags").unwrap(), i % 2 == 0);
        m.set_message(ty.field("inners").unwrap())
            .set_str(inner_ty.field("name").unwrap(), "grandkid")
            .set_i64(inner_ty.field("value").unwrap(), i as i64 + 1);
    }
    m
}

/// Floor iterations per tier; doubled until one pass takes 0.25 s.
const MIN_PASS_SECONDS: f64 = 0.25;

fn calibrate<T, F: FnMut() -> T>(op: &mut F, floor: usize) -> usize {
    let mut count = floor.max(1);
    loop {
        let start = Instant::now();
        for _ in 0..count {
            std::hint::black_box(op());
        }
        if start.elapsed().as_secs_f64() >= MIN_PASS_SECONDS || count >= 100_000_000 {
            return count;
        }
        count *= 2;
    }
}

fn run<T, F: FnMut() -> T>(op: &mut F, iterations: usize) -> f64 {
    let start = Instant::now();
    for _ in 0..iterations {
        std::hint::black_box(op());
    }
    start.elapsed().as_nanos() as f64 / iterations as f64
}

/* Every op calibrates independently from the floor (a slow op already exceeds
 * the minimum at the floor and keeps it), then warms up and keeps the best
 * of PASSES runs. */
fn measure<T, F: FnMut() -> T>(floor: usize, mut op: F) -> f64 {
    let count = calibrate(&mut op, floor);
    run(&mut op, count);
    let mut best = f64::MAX;
    for _ in 0..PASSES {
        best = best.min(run(&mut op, count));
    }
    best
}

fn row(impl_name: &str, op: &str, bytes: usize, ns: f64) {
    println!(
        "  {:<12} {:<7} {:>12.1} {:>10.1}",
        impl_name,
        op,
        ns,
        bytes as f64 / ns * 1000.0
    );
}

fn fail(label: &str, what: &str) {
    println!("FAIL {label}: {what}");
    unsafe { FAILURES += 1 };
}

/// The in-place fixture: the flat image plus every allocation it points into,
/// built once per tier so the timed loop only runs the codec.
struct FlatPayload {
    image: Scalars3Flat,
    _inner: Box<Inner3Flat>,
    _inners: Vec<Inner3Flat>,
    _nums: Vec<i32>,
    _tags: Vec<*mut c_char>,
    _flags: Vec<u8>,
    _strs: Vec<CString>,
}

impl FlatPayload {
    fn new(n: usize) -> FlatPayload {
        let mut strs: Vec<CString> = ["hello", "kid", "opt", "tag", "grandkid"]
            .iter()
            .map(|s| CString::new(*s).expect("cstring"))
            .collect();
        let ptr = |i: usize| strs[i].as_ptr() as *mut c_char;
        let mut inner = Box::new(Inner3Flat {
            name: ptr(1),
            value: 42,
        });
        let mut nums: Vec<i32> = (0..n).map(|i| i as i32 - 3).collect();
        let mut tags: Vec<*mut c_char> = (0..n).map(|_| ptr(3)).collect();
        let mut inners: Vec<Inner3Flat> = (0..n)
            .map(|i| Inner3Flat {
                name: ptr(4),
                value: i as i32 + 1,
            })
            .collect();
        let mut flags: Vec<u8> = (0..n).map(|i| u8::from(i % 2 == 0)).collect();
        let image = Scalars3Flat {
            i32: i32::MIN,
            i64: i64::MAX,
            u32: u32::MAX,
            u64: u64::MAX,
            s32: i32::MIN,
            s64: i64::MIN,
            fx32: 0xdeadbeef,
            fx64: 72623859790382856,
            sfx32: i32::MIN,
            sfx64: i64::MIN,
            f32: f32::INFINITY,
            f64: -123456.789,
            flag: 1,
            str: ptr(0),
            color: Color3::COLOR3_GREEN as i32,
            inner: &mut *inner as *mut Inner3Flat,
            nums: nums.as_mut_ptr(),
            nums_count: nums.len(),
            tags: tags.as_mut_ptr(),
            tags_count: tags.len(),
            inners: inners.as_mut_ptr(),
            inners_count: inners.len(),
            opt_i32: -7,
            has_opt_i32: 1,
            opt_str: ptr(2),
            flags: flags.as_mut_ptr(),
            flags_count: flags.len(),
        };
        FlatPayload {
            image,
            _inner: inner,
            _inners: inners,
            _nums: nums,
            _tags: tags,
            _flags: flags,
            _strs: std::mem::take(&mut strs),
        }
    }
}
