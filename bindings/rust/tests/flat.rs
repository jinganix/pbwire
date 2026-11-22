//! The in-place arm's gate (plan 08, Stage 3): the generated `#[repr(C)]`
//! mirror is the native object and one call into the C struct codec encodes
//! or decodes it.
//!
//! Asserts, on the pinned `matrix3` fixture, that the flat image's bytes equal
//! both the official protoc reference (`c/tests/data/matrix3.bin`) and the
//! pinned prost oracle (`gen_oracle/matrix3.rs`), that a decode exposes every
//! fixture value (strings, bytes-free scalars, the sub-message, the repeated
//! fields and the proto3 optionals), and that the decoded image re-encodes to
//! the same bytes.
//!
//! Run: `cargo test --test flat` (from bindings/rust).

#[path = "gen/matrix3_pbwire.rs"]
mod matrix3;

mod oracle3 {
    #![allow(dead_code)]
    include!("gen_oracle/matrix3.rs");
}

use std::ffi::{c_char, CStr, CString};

use matrix3::{Inner3Flat, Scalars3Flat};
use pbwire::{FlatImage, Schema};

const DESC: &str = "../c/tests/data/matrix3.desc";
const ORACLE: &[u8] = include_bytes!("../../c/tests/data/matrix3.bin");

/// The flat fixture: the image plus every allocation the image points into, so
/// the pointers stay valid for the whole test.
struct Fixture {
    image: Scalars3Flat,
    _inner: Box<Inner3Flat>,
    _inners: Vec<Inner3Flat>,
    _nums: Vec<i32>,
    _tags: Vec<*mut c_char>,
    _flags: Vec<u8>,
    _strs: Vec<CString>,
}

fn fixture() -> Fixture {
    let mut strs: Vec<CString> = ["hello", "kid", "opt", "a", "bb", "x", "y"]
        .iter()
        .map(|s| CString::new(*s).expect("cstring"))
        .collect();
    let ptr = |i: usize| strs[i].as_ptr() as *mut c_char;
    let mut inner = Box::new(Inner3Flat {
        name: ptr(1),
        value: 42,
    });
    let mut inners = vec![
        Inner3Flat {
            name: ptr(5),
            value: 1,
        },
        Inner3Flat {
            name: ptr(6),
            value: -2,
        },
    ];
    let mut nums: Vec<i32> = vec![1, -5, 300];
    let mut tags: Vec<*mut c_char> = vec![ptr(3), ptr(4)];
    let mut flags: Vec<u8> = vec![1, 0, 1];
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
        color: 2,
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
    Fixture {
        image,
        _inner: inner,
        _inners: inners,
        _nums: nums,
        _tags: tags,
        _flags: flags,
        _strs: std::mem::take(&mut strs),
    }
}

fn oracle() -> oracle3::Scalars3 {
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

fn cstring(ptr: *mut c_char) -> String {
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .expect("utf8")
        .to_string()
}

#[test]
fn flat_image_matches_both_oracles() {
    use prost::Message as _;
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let mut plan = schema.in_place::<Scalars3Flat>().expect("compile in place");
    assert_eq!(plan.name(), "structtest.Scalars3");
    assert_eq!(Scalars3Flat::FIELDS.len(), 22, "one row per schema field");

    let fixture = fixture();
    let bytes = plan.encode_in_place(&fixture.image).expect("encode");
    assert_eq!(bytes, ORACLE, "flat image != protoc reference");
    assert_eq!(bytes, oracle().encode_to_vec(), "flat image != prost");
}

#[test]
fn flat_image_decodes_every_field() {
    let schema = Schema::from_file(DESC).expect("load descriptor set");
    let mut plan = schema.in_place::<Scalars3Flat>().expect("compile in place");
    let mut image = FlatImage::<Scalars3Flat>::new();
    plan.decode_in_place(ORACLE, &mut image).expect("decode");
    let v = &image.value;
    assert_eq!(v.i32, i32::MIN);
    assert_eq!(v.i64, i64::MAX);
    assert_eq!(v.u32, u32::MAX);
    assert_eq!(v.u64, u64::MAX);
    assert_eq!(v.s32, i32::MIN);
    assert_eq!(v.s64, i64::MIN);
    assert_eq!(v.fx32, 0xdeadbeef);
    assert_eq!(v.fx64, 72623859790382856);
    assert_eq!(v.sfx32, i32::MIN);
    assert_eq!(v.sfx64, i64::MIN);
    assert_eq!(v.f32, f32::INFINITY);
    assert_eq!(v.f64, -123456.789);
    assert_eq!(v.flag, 1);
    assert_eq!(v.color, 2);
    assert_eq!(cstring(v.str), "hello");
    let inner = unsafe { &*v.inner };
    assert_eq!(cstring(inner.name), "kid");
    assert_eq!(inner.value, 42);
    assert_eq!(v.nums_count, 3);
    assert_eq!(unsafe { *v.nums.add(2) }, 300);
    assert_eq!(v.tags_count, 2);
    assert_eq!(cstring(unsafe { *v.tags.add(1) }), "bb");
    assert_eq!(v.inners_count, 2);
    let last = unsafe { &*v.inners.add(1) };
    assert_eq!(cstring(last.name), "y");
    assert_eq!(last.value, -2);
    assert_eq!(v.flags_count, 3);
    assert_eq!(unsafe { *v.flags.add(1) }, 0);
    assert_eq!(v.has_opt_i32, 1);
    assert_eq!(v.opt_i32, -7);
    assert_eq!(cstring(v.opt_str), "opt");
    /* The decoded image re-encodes to the same bytes (round trip). */
    assert_eq!(plan.encode_in_place(&image.value).expect("re-encode"), ORACLE);
}
