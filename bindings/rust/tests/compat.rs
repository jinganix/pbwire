//! Typed-compat check for the Rust binding (TESTSPEC T1–T6):
//!
//! T1  generated `encode_to_vec` == pinned protoc reference bytes
//! T2  `decode` of the reference reads every fixture value back
//!     (literals mirror `bindings/typed/testdata/vectors.json`; the
//!     binary reference is the authority, asserted byte-for-byte in T1)
//! T3  `default()` encodes empty; explicit optional zero round-trips
//! T4  differential vs the pinned prost oracle: same bytes, same fields,
//!     cross-decode agrees
//! T5  one generic workflow runs against both libraries' messages with
//!     identical observable results (import swap only)
//! T6  errors are loud, not crashes: bad wire fails on both sides
//!
//! Run: `cargo test --test compat` (from bindings/rust).

#[path = "gen/matrix3_pbwire.rs"]
mod matrix3;
// Renamed: the file itself contains `pub mod oneof1`, so `mod oneof1` would
// nest it as oneof1::oneof1::Kind.
#[path = "gen/oneof1_pbwire.rs"]
mod spectest;

mod oracle3 {
    #![allow(dead_code)]
    include!("gen_oracle/matrix3.rs");
}
mod oracle1 {
    #![allow(dead_code)]
    include!("gen_oracle/oneof1.rs");
}

use matrix3::{Color3, Inner3, Scalars3};

const ORACLE: &[u8] = include_bytes!("../../c/tests/data/matrix3.bin");
const ORACLE_ONEOF: &[u8] = include_bytes!("../../typed/testdata/oneof1.bin");

fn build() -> Scalars3 {
    Scalars3 {
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
        str: "hello".into(),
        color: Color3::Green as i32,
        inner: Some(Inner3 {
            name: "kid".into(),
            value: 42,
        }),
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
        opt_i32: Some(-7),
        opt_str: Some("opt".into()),
        flags: vec![true, false, true],
    }
}

fn build_oracle() -> oracle3::Scalars3 {
    oracle3::Scalars3 {
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
        str: "hello".into(),
        color: 2,
        inner: Some(oracle3::Inner3 {
            name: "kid".into(),
            value: 42,
        }),
        nums: vec![1, -5, 300],
        tags: vec!["a".into(), "bb".into()],
        inners: vec![
            oracle3::Inner3 {
                name: "x".into(),
                value: 1,
            },
            oracle3::Inner3 {
                name: "y".into(),
                value: -2,
            },
        ],
        opt_i32: Some(-7),
        opt_str: Some("opt".into()),
        flags: vec![true, false, true],
    }
}

#[test]
fn t1_encode_matches_protoc() {
    assert_eq!(build().encode_to_vec(), ORACLE, "T1 facade != reference");
}

#[test]
fn t2_decode_reads_every_field() {
    let back = Scalars3::decode(ORACLE).expect("T2 decode");
    assert_eq!(back.i32, i32::MIN);
    assert_eq!(back.i64, i64::MAX);
    assert_eq!(back.u32, u32::MAX);
    assert_eq!(back.u64, u64::MAX);
    assert_eq!(back.s32, i32::MIN);
    assert_eq!(back.s64, i64::MIN);
    assert_eq!(back.fx32, 0xdeadbeef);
    assert_eq!(back.fx64, 72623859790382856);
    assert_eq!(back.sfx32, i32::MIN);
    assert_eq!(back.sfx64, i64::MIN);
    assert!(back.f32.is_infinite() && back.f32.is_sign_positive());
    assert_eq!(back.f64, -123456.789);
    assert!(back.flag);
    assert_eq!(back.str, "hello");
    assert_eq!(back.color, 2);
    assert_eq!(Color3::from_str_name("COLOR3_GREEN"), Some(Color3::Green));
    let inner = back.inner.expect("T2 inner present");
    assert_eq!((inner.name, inner.value), ("kid".to_string(), 42));
    assert_eq!(back.nums, vec![1, -5, 300]);
    assert_eq!(back.tags, vec!["a".to_string(), "bb".to_string()]);
    assert_eq!(back.inners.len(), 2);
    assert_eq!(back.inners[0].name, "x");
    assert_eq!(back.inners[1].value, -2);
    assert_eq!((back.opt_i32, back.opt_str.as_deref()), (Some(-7), Some("opt")));
    assert_eq!(back.flags, vec![true, false, true]);

    let one = spectest::Oneof1 {
        name: "a".into(),
        kind: Some(spectest::oneof1::Kind::Text("hi".into())),
    };
    assert_eq!(one.encode_to_vec(), ORACLE_ONEOF, "T1 oneof != reference");
    let ob = spectest::Oneof1::decode(ORACLE_ONEOF).expect("T2 oneof decode");
    assert_eq!(ob.name, "a");
    assert!(matches!(ob.kind, Some(spectest::oneof1::Kind::Text(ref s)) if s == "hi"));
    let nested = spectest::Oneof1 {
        name: "n".into(),
        kind: Some(spectest::oneof1::Kind::Inner(spectest::oneof1::Inner { v: 7 })),
    };
    let nb = spectest::Oneof1::decode(&nested.encode_to_vec()).expect("nested oneof");
    assert!(matches!(nb.kind, Some(spectest::oneof1::Kind::Inner(ref m)) if m.v == 7));
}

#[test]
fn t3_empty_and_explicit_zero() {
    assert!(Scalars3::default().encode_to_vec().is_empty(), "T3 empty encodes empty");
    let zero = Scalars3 {
        opt_i32: Some(0),
        ..Scalars3::default()
    };
    let back = Scalars3::decode(&zero.encode_to_vec()).expect("T3 round trip");
    assert_eq!(back.opt_i32, Some(0), "T3 explicit zero keeps presence");
    assert_eq!(back.opt_str, None);
}

#[test]
fn t4_oracle_agrees() {
    use prost::Message as _;
    let ow = build_oracle().encode_to_vec();
    assert_eq!(ow, ORACLE, "T4 oracle != reference");
    // Cross-decode: each library parses the other's bytes to the same fields.
    let om = oracle3::Scalars3::decode(build().encode_to_vec().as_slice()).expect("oracle reads ours");
    let fm = Scalars3::decode(&ow).expect("we read oracle's");
    assert_eq!((om.i32, om.i64, om.u32, om.u64), (fm.i32, fm.i64, fm.u32, fm.u64));
    assert_eq!((om.s32, om.s64, om.fx32, om.fx64), (fm.s32, fm.s64, fm.fx32, fm.fx64));
    assert_eq!((om.flag, om.str.clone(), om.color), (fm.flag, fm.str.clone(), fm.color));
    assert_eq!(om.inner.as_ref().unwrap().name, fm.inner.as_ref().unwrap().name);
    assert_eq!((om.opt_i32, om.opt_str), (fm.opt_i32, fm.opt_str));
    assert_eq!((om.nums.len(), om.inners.len()), (fm.nums.len(), fm.inners.len()));

    let po = oracle1::Oneof1 {
        name: "a".into(),
        kind: Some(oracle1::oneof1::Kind::Text("hi".into())),
    };
    assert_eq!(po.encode_to_vec(), ORACLE_ONEOF, "T4 oracle oneof != reference");
}

/// The comparable business result for T5.
#[derive(PartialEq, Debug)]
struct Summary {
    name: String,
    i32: i32,
    nums: Vec<i32>,
    inner: String,
    has_opt: bool,
}

/// Written once against abstract operations; M instantiates it per library,
// so both run literally the same steps.
fn workflow<M>(
    new: impl Fn() -> M,
    set: impl Fn(&mut M),
    enc: impl Fn(&M) -> Vec<u8>,
    dec: impl Fn(&[u8]) -> M,
    get: impl Fn(&M) -> Summary,
) -> (Vec<u8>, Summary) {
    let mut m = new();
    set(&mut m);
    let wire = enc(&m);
    let back = dec(&wire);
    (wire, get(&back))
}

#[test]
fn t5_same_business_code() {
    use prost::Message as _;
    let a = workflow(
        oracle3::Scalars3::default,
        |m: &mut oracle3::Scalars3| {
            m.str = "kid".into();
            m.i32 = -5;
            m.nums = vec![1, 2];
            m.inner = Some(oracle3::Inner3 {
                name: "x".into(),
                value: 1,
            });
        },
        |m| m.encode_to_vec(),
        |b| oracle3::Scalars3::decode(b).unwrap(),
        |m| Summary {
            name: m.str.clone(),
            i32: m.i32,
            nums: m.nums.clone(),
            inner: m.inner.as_ref().map(|i| i.name.clone()).unwrap_or_default(),
            has_opt: m.opt_i32.is_some(),
        },
    );
    let b = workflow(
        Scalars3::default,
        |m: &mut Scalars3| {
            m.str = "kid".into();
            m.i32 = -5;
            m.nums = vec![1, 2];
            m.inner = Some(Inner3 {
                name: "x".into(),
                value: 1,
            });
        },
        |m| m.encode_to_vec(),
        |b| Scalars3::decode(b).unwrap(),
        |m| Summary {
            name: m.str.clone(),
            i32: m.i32,
            nums: m.nums.clone(),
            inner: m.inner.as_ref().map(|i| i.name.clone()).unwrap_or_default(),
            has_opt: m.opt_i32.is_some(),
        },
    );
    assert_eq!(a, b, "T5 mismatch: same steps, different results");
}

#[test]
fn t6_errors_are_loud() {
    use prost::Message as _;
    assert!(Scalars3::decode(&[0xff, 0xff, 0xff]).is_err(), "T6 bad wire fails");
    assert!(oracle3::Scalars3::decode(&[0xff, 0xff, 0xff][..]).is_err());
    assert!(Scalars3::decode(&[]).is_ok(), "T6 empty decodes to default");
    assert_eq!(Scalars3::decode(&[]).unwrap(), Scalars3::default());
    let one = spectest::Oneof1 {
        name: "n".into(),
        kind: Some(spectest::oneof1::Kind::Num(1)),
    };
    let back = spectest::Oneof1::decode(&one.encode_to_vec()).expect("T6 oneof member");
    assert!(matches!(back.kind, Some(spectest::oneof1::Kind::Num(1))));
}
