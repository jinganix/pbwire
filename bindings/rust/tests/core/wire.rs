//! Wire-primitive gate for the pure-Rust core: the reader's masking, the strict
//! length and tag rules, the group skipper's depth cap and a deterministic
//! "garbage never panics" sweep. The primitives' own unit tests live next to
//! them in `src/core/wire.rs`; this file is the behaviour the codec relies on.

use std::path::PathBuf;

use pbwire::core::{
    decode, decode_slice, encode, CoreError, Limits, MessageType, Reader, Schema, MAX_DEPTH,
};

fn data_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../c/tests/data")
}

fn type_of(file: &str, name: &str) -> MessageType {
    Schema::from_file(data_dir().join(file))
        .expect("descriptor")
        .message(name)
        .expect("message")
}

fn status_of<T>(r: Result<T, CoreError>) -> i32 {
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

#[test]
fn varint32_keeps_its_low_32_bits() {
    // A 10-byte encoding of -1: the 32-bit reader returns the low 32 bits,
    // like the C core's uint32_t.
    let ten = varint(u64::MAX);
    assert_eq!(ten.len(), 10);
    let mut r = Reader::new(&ten);
    assert_eq!(r.r_var32().unwrap(), u32::MAX);
    let mut r = Reader::new(&ten);
    assert_eq!(r.r_var64().unwrap(), u64::MAX);
    // An 11-byte varint is an overflow, not a truncation.
    // Ten continuation bytes and one more: past the 64-bit shift.
    let mut eleven = vec![0xffu8; 10];
    eleven.push(0x01);
    let mut r = Reader::new(&eleven);
    assert!(r.r_var64().is_err(), "varint overflow");
    // A non-minimal (padded) encoding of 1 is accepted.
    let padded = [0x81u8, 0x80, 0x80, 0x80, 0x00];
    let mut r = Reader::new(&padded);
    assert_eq!(r.r_var32().unwrap(), 1);
    assert_eq!(r.pos, 5);
}

#[test]
fn lengths_are_strict() {
    // A five-byte length whose fifth byte carries more than four bits is out of
    // range: rejected, not truncated.
    let bad = [0x80u8, 0x80, 0x80, 0x80, 0x10];
    let mut r = Reader::new(&bad);
    assert!(r.r_len32().is_err(), "length overflow");
    // Six bytes: past the five-byte cap.
    let six = [0x80u8, 0x80, 0x80, 0x80, 0x80, 0x01];
    let mut r = Reader::new(&six);
    assert!(r.r_len32().is_err(), "bad length");
    // A length that runs past the buffer.
    let over = [0x05u8, 0x01];
    let mut r = Reader::new(&over);
    assert!(r.r_len32().is_err(), "truncated length-delimited");
    // The widest legal length is 2^32-1 in five bytes; the buffer check fires,
    // not the shape check.
    let wide = [0xffu8, 0xff, 0xff, 0xff, 0x0f];
    let mut r = Reader::new(&wide);
    assert!(r.r_len32().is_err(), "length past the buffer");
}

#[test]
fn tags_are_strict_but_truncate_like_the_reference() {
    let mut r = Reader::new(&[]);
    assert!(r.r_tag().is_err(), "truncated tag");
    // Six-byte tag: rejected.
    let six = [0x80u8, 0x80, 0x80, 0x80, 0x80, 0x01];
    let mut r = Reader::new(&six);
    assert!(r.r_tag().is_err(), "bad tag");
    // A 5-byte tag above UINT32_MAX truncates to its low 32 bits.
    let wide = [0x88u8, 0x80, 0x80, 0x80, 0x10];
    let mut r = Reader::new(&wide);
    assert_eq!(r.r_tag().unwrap(), 8);
}

#[test]
fn group_skipping_respects_the_depth_cap() {
    // 300 nested unknown groups: past the cap, so the skip fails with status 4.
    let mut wire = Vec::new();
    let depth = MAX_DEPTH as usize + 5;
    for _ in 0..depth {
        wire.extend(varint(9 * 8 + 3));
    }
    for _ in 0..depth {
        wire.extend(varint(9 * 8 + 4));
    }
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    assert_eq!(status_of(ty.decode(&wire)), 4, "nesting exceeds the cap");
    // A shallow group skip succeeds and is retained as one unknown.
    let ty = type_of("grouptest.desc", "structtest.GroupTest");
    let mut wire = varint(9 * 8 + 3);
    wire.extend(varint(1 * 8));
    wire.extend(varint(3));
    wire.extend(varint(9 * 8 + 4));
    let m = ty.decode(&wire).expect("unknown group");
    assert_eq!(m.unknown().len(), 1);
}

#[test]
fn garbage_never_panics() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let gty = type_of("grouptest.desc", "structtest.GroupTest");
    // A small deterministic PRNG: the point is coverage of every tag/length
    // shape, not statistics.
    let mut state: u64 = 0x2545_f491_4f6c_dd1d;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for round in 0..4000 {
        let len = (next() % 48) as usize;
        let mut wire = Vec::with_capacity(len);
        for _ in 0..len {
            wire.push((next() & 0xff) as u8);
        }
        // Whatever comes out, it is a value or an error -- never a panic.
        let _ = ty.decode(&wire);
        let _ = gty.decode(&wire);
        if round % 7 == 0 {
            let _ = ty.decode_slice(&wire, 0, wire.len(), &Limits::default());
        }
        // A tree decodes back to bytes and re-decodes to the same tree bytes.
        if let Ok(m1) = ty.decode(&wire) {
            let bytes = encode(&m1).expect("encode of a decoded tree");
            if let Ok(m2) = ty.decode(&bytes) {
                assert_eq!(encode(&m2).unwrap(), bytes, "round {round}: not stable");
            }
        }
    }
}

#[test]
fn decode_slice_reads_a_body_inside_a_larger_buffer() {
    let ty = type_of("matrix3.desc", "structtest.Scalars3");
    let mut wire = vec![0xde, 0xad];
    wire.extend(varint(1 * 8) /* i32 */);
    wire.extend(varint(7));
    wire.extend_from_slice(&[0xbe, 0xef]);
    let limits = Limits::default();
    let m = decode_slice(ty.schema(), ty.def_index(), &wire, 2, 2, &limits).expect("slice");
    assert_eq!(m.get_i64("i32").unwrap(), Some(7));
    assert!(decode(ty.schema(), ty.def_index(), &wire[2..4], &limits).is_ok());
}
