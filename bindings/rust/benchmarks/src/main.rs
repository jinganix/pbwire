/* Rust binding benchmark against rust-protobuf's generated code on the same
 * matrix3.proto and the four standard tiers from BENCHMARKS.md.
 *
 * The same payload as the C/Go/Java rows: every scalar kind, the nested
 * message, the two proto3 optionals, and the repeated fields scaled x0 / x1 /
 * x100 / x3000, which lands the tiers on 136 / 174 / 2469 / 74913 bytes. Both
 * sides must produce identical bytes before anything is timed, and both sides
 * allocate per call (pbwire encode()/decode() against write_to_bytes()/
 * parse_from_bytes()), so the comparison is like for like.
 *
 *   ./bench.sh          generate, build, run
 */

#[path = "../gen/matrix3.rs"]
mod matrix3;

use std::time::Instant;

use matrix3::{Color3, Inner3, Scalars3};
use pbwire::{Message, Schema, Type};
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

    for tier in 0..CASES.len() {
        run_case(&ty, &inner_ty, tier);
    }
    if unsafe { FAILURES } != 0 {
        eprintln!("{} check(s) failed", unsafe { FAILURES });
        std::process::exit(1);
    }
}

fn run_case(ty: &Type<'_>, inner_ty: &Type<'_>, tier: usize) {
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

    let official = fill_official(n);
    let encode = measure(iterations, || message.encode().expect("encode").len());
    let decode = measure(iterations, || ty.decode(&reference).expect("decode"));
    let official_encode = measure(iterations, || official.write_to_bytes().expect("encode").len());
    let official_decode = measure(iterations, || {
        Scalars3::parse_from_bytes(&reference).expect("decode")
    });

    println!(
        "{label}: {} bytes, {iterations} iterations (repeat field x{n})",
        reference.len()
    );
    println!("  {:<12} {:<7} {:>12} {:>10}", "impl", "op", "ns/op", "MB/s");
    row("pbwire-rust", "encode", reference.len(), encode);
    row("protobuf-rust", "encode", reference.len(), official_encode);
    row("pbwire-rust", "decode", reference.len(), decode);
    row("protobuf-rust", "decode", reference.len(), official_decode);
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

fn run<T, F: FnMut() -> T>(op: &mut F, iterations: usize) -> f64 {
    let start = Instant::now();
    for _ in 0..iterations {
        std::hint::black_box(op());
    }
    start.elapsed().as_nanos() as f64 / iterations as f64
}

/* Warm up, then keep the best of PASSES runs. */
fn measure<T, F: FnMut() -> T>(iterations: usize, mut op: F) -> f64 {
    run(&mut op, iterations);
    let mut best = f64::MAX;
    for _ in 0..PASSES {
        best = best.min(run(&mut op, iterations));
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
