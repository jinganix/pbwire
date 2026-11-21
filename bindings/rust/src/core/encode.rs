//! Encoder: one pass into a buffer, fields in schema (field-number) order,
//! unknown fields appended verbatim in input order. Nested payloads use a
//! one-byte length placeholder patched afterwards and packed runs use a
//! five-byte placeholder shifted down once, so a decode/encode round trip is
//! byte-exact.
//!
//! The Rust twin of `bindings/python/pure/encode.py`.

use super::descriptor::{Field, Kind, Presence};
use super::tree::{Message, Value};
use super::wire::{
    fail_status, size_var32, w_bytes, w_fixed32, w_fixed64, w_len, w_tag, w_tag_fixed32,
    w_tag_fixed64, w_tag_var32, w_tag_var64, w_var32, w_var64, zig32, zig64, CoreError,
    Result, MAX_DEPTH,
};

/// Encodes a tree into a fresh buffer.
pub fn encode(m: &Message) -> Result<Vec<u8>> {
    let mut w = Vec::with_capacity(256);
    encode_into(m, &mut w, 0)?;
    Ok(w)
}

/// Encodes a tree into a reusable buffer (cleared first): a hot loop pays no
/// allocation per op.
pub fn encode_to(m: &Message, w: &mut Vec<u8>) -> Result<()> {
    w.clear();
    encode_into(m, w, 0)
}

/// Encodes a tree body into the buffer, appending.
pub fn encode_into(m: &Message, w: &mut Vec<u8>, depth: u32) -> Result<()> {
    if depth > MAX_DEPTH {
        return fail_status(format!("nesting exceeds {MAX_DEPTH}"), 4);
    }
    let def = m.def();
    let no_required = !def.has_required; // proto3: skip the check
    for (slot, f) in def.fields.iter().enumerate() {
        let v = match &m.v[slot] {
            Some(v) => v,
            None => {
                if !no_required && f.presence == Presence::Required {
                    return Err(CoreError::with_status(
                        format!("missing required field {}.{}", def.full_name, f.name),
                        6,
                    ));
                }
                continue;
            }
        };
        if f.repeated {
            let items = match v {
                Value::List(items) => items.as_slice(),
                _ => &[],
            };
            if items.is_empty() {
                continue;
            }
            if f.kind.is_packable() {
                encode_rep_num(w, f, items)?;
            } else {
                for item in items {
                    write_value(w, f, item, depth)?;
                }
            }
            continue;
        }
        if f.kind.is_packable() {
            // Only implicit defaults skip here; explicit defaults (and
            // required) emit once set.
            if is_zero(v) && f.presence == Presence::Implicit {
                continue;
            }
            write_singular_num(w, f, v);
            continue;
        }
        // Implicit empty strings and bytes stay silent, everything else emits.
        if f.presence == Presence::Implicit {
            if let Value::Bytes(b) = v {
                if b.is_empty() {
                    continue;
                }
            }
        }
        write_value(w, f, v, depth)?;
    }
    // Unknown fields last, in input order.
    for u in &m.unknown {
        w.extend_from_slice(u);
    }
    Ok(())
}

fn is_zero(v: &Value) -> bool {
    match v {
        Value::Bool(b) => !*b,
        Value::I32(x) => *x == 0,
        Value::U32(x) => *x == 0,
        Value::I64(x) => *x == 0,
        Value::U64(x) => *x == 0,
        Value::F32(x) => *x == 0,
        Value::F64(x) => *x == 0,
        _ => false,
    }
}

fn write_value(w: &mut Vec<u8>, f: &Field, v: &Value, depth: u32) -> Result<()> {
    match f.kind {
        Kind::String | Kind::Bytes => {
            let data = match v {
                Value::Bytes(b) => b.as_slice(),
                _ => &[],
            };
            w_bytes(w, &f.tag, data);
            Ok(())
        }
        Kind::Message => {
            // Inlined tag + length placeholder + patch: a repeated message
            // field is the hottest nesting shape.
            let sub = match v {
                Value::Msg(m) => m,
                _ => return Ok(()),
            };
            w.reserve(f.tag.as_slice().len() + 1);
            w_tag(w, &f.tag);
            let lp = w.len();
            w.push(0);
            encode_into(sub, w, depth + 1)?;
            super::wire::patch_len(w, lp);
            Ok(())
        }
        Kind::Group => {
            // Start tag, body, end tag.
            let sub = match v {
                Value::Msg(m) => m,
                _ => return Ok(()),
            };
            w_tag(w, &f.tag);
            encode_into(sub, w, depth + 1)?;
            w_tag(w, &f.tag_end);
            Ok(())
        }
        _ => Ok(()),
    }
}

/// Singular numeric write: one fused tag+value call per field.
fn write_singular_num(w: &mut Vec<u8>, f: &Field, v: &Value) {
    let i = v.as_i64().unwrap_or(0);
    let u = v.as_u64().unwrap_or(0);
    match f.kind {
        // Encode the int32 interpretation, sign-extended: the reference
        // truncates a wide varint to int32 and re-emits a negative as 10 bytes.
        Kind::Int32 | Kind::Enum => {
            if i >= 0 {
                w_tag_var32(w, &f.tag, i as u32);
            } else {
                w_tag_var64(w, &f.tag, i as u64);
            }
        }
        Kind::Uint32 => w_tag_var32(w, &f.tag, u as u32),
        Kind::Sint32 => w_tag_var32(w, &f.tag, zig32(i as i32)),
        Kind::Int64 | Kind::Uint64 => w_tag_var64(w, &f.tag, u),
        Kind::Sint64 => w_tag_var64(w, &f.tag, zig64(i)),
        Kind::Bool => w_tag_var32(w, &f.tag, u32::from(u != 0)),
        Kind::Fixed32 | Kind::Sfixed32 | Kind::Float => w_tag_fixed32(w, &f.tag, u as u32),
        // Fixed64, Sfixed64, Double
        _ => w_tag_fixed64(w, &f.tag, u),
    }
}

/// One dispatch per field; the loops below it are branch-free writes.
fn encode_rep_num(w: &mut Vec<u8>, f: &Field, items: &[Value]) -> Result<()> {
    let n = items.len();
    if f.packed {
        match f.kind {
            Kind::Bool => {
                w_tag(w, &f.tag);
                w_len(w, n);
                w.reserve(n);
                for v in items {
                    w.push(u8::from(v.as_bool().unwrap_or(false)));
                }
                return Ok(());
            }
            Kind::Fixed32 | Kind::Sfixed32 | Kind::Float => {
                w_tag(w, &f.tag);
                w_len(w, n * 4);
                w.reserve(n * 4);
                for v in items {
                    w_fixed32(w, v.as_u64().unwrap_or(0) as u32);
                }
                return Ok(());
            }
            Kind::Fixed64 | Kind::Sfixed64 | Kind::Double => {
                w_tag(w, &f.tag);
                w_len(w, n * 8);
                w.reserve(n * 8);
                for v in items {
                    w_fixed64(w, v.as_u64().unwrap_or(0));
                }
                return Ok(());
            }
            _ => {
                // Varint kinds: write the payload over a 5-byte placeholder,
                // then shift it down: one memmove beats a size pre-pass over
                // thousands of elements.
                w_tag(w, &f.tag);
                let lp = w.len();
                w.reserve(n * 10 + 5);
                w.extend_from_slice(&[0u8; 5]);
                for v in items {
                    write_packed_var(w, f.kind, v);
                }
                let length = w.len() - lp - 5;
                let llen = size_var32(length as u64);
                if llen != 5 {
                    let written = w.len();
                    w.copy_within(lp + 5..written, lp + llen);
                    w.truncate(lp + llen + length);
                }
                let mut p = lp;
                let mut x = length as u64;
                while x > 0x7f {
                    w[p] = (x & 0x7f) as u8 | 0x80;
                    p += 1;
                    x >>= 7;
                }
                w[p] = x as u8;
                return Ok(());
            }
        }
    }
    for v in items {
        w_tag(w, &f.tag);
        write_num_single(w, f.kind, v);
    }
    Ok(())
}

fn write_packed_var(w: &mut Vec<u8>, kind: Kind, v: &Value) {
    write_num_single(w, kind, v)
}

fn write_num_single(w: &mut Vec<u8>, kind: Kind, v: &Value) {
    let i = v.as_i64().unwrap_or(0);
    let u = v.as_u64().unwrap_or(0);
    match kind {
        Kind::Int32 | Kind::Enum => {
            if i >= 0 {
                w_var32(w, i as u32);
            } else {
                w_var64(w, i as u64);
            }
        }
        Kind::Uint32 => w_var32(w, u as u32),
        Kind::Sint32 => w_var32(w, zig32(i as i32)),
        Kind::Int64 | Kind::Uint64 => w_var64(w, u),
        Kind::Sint64 => w_var64(w, zig64(i)),
        Kind::Bool => w.push(u8::from(u != 0)),
        Kind::Fixed32 | Kind::Sfixed32 | Kind::Float => w_fixed32(w, u as u32),
        // Fixed64, Sfixed64, Double
        _ => w_fixed64(w, u),
    }
}
