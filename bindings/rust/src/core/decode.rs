//! Decoder: strict tags and lengths (like the C core), unknown field numbers
//! AND wrong-wire-type occurrences retained as raw tag+payload bytes in input
//! order, groups decoded to their matching end tag, packed runs accepted for
//! every numeric kind (and the unpacked element form for a packed field),
//! oneofs collapsed to the last member, presence following the field's own
//! zero, proto2 required fields checked over the whole tree, and the nesting cap
//! enforced on every recursive path.
//!
//! The Rust twin of `bindings/python/pure/decode.py`.

use std::rc::Rc;

use super::descriptor::{CoreSchema, Field, Kind, Limits, Presence};
use super::tree::{Message, Value};
use super::wire::{
    fail_status, unzig32, unzig64, utf8_valid, CoreError, Reader, Result, WT_END_GROUP,
    WT_FIXED32, WT_LEN, WT_START_GROUP,
};

/// Decodes a whole message, unknown fields retained.
pub fn decode(
    schema: &Rc<CoreSchema>,
    def: usize,
    data: &[u8],
    limits: &Limits,
) -> Result<Message> {
    limits.check_bytes(data.len(), "message")?;
    let mut r = Reader::new(data);
    r.cap = limits.depth();
    let mut m = Message::new(Rc::clone(schema), def);
    decode_into(&mut m, &mut r, data.len(), -1, 0)?;
    if m.def().deep_required {
        let cap = r.cap;
        check_required(&m, cap, 0)?;
    }
    Ok(m)
}

/// Decodes a message body that already starts at `off` and ends at
/// `off + length`.
pub fn decode_slice(
    schema: &Rc<CoreSchema>,
    def: usize,
    data: &[u8],
    off: usize,
    length: usize,
    limits: &Limits,
) -> Result<Message> {
    limits.check_bytes(length, "message")?;
    let end = off.checked_add(length).ok_or_else(|| CoreError::new("bad slice"))?;
    let mut r = Reader::slice(data, off, end)?;
    r.cap = limits.depth();
    let mut m = Message::new(Rc::clone(schema), def);
    decode_into(&mut m, &mut r, end, -1, 0)?;
    if m.def().deep_required {
        let cap = r.cap;
        check_required(&m, cap, 0)?;
    }
    Ok(m)
}

/// True when the field does not accept this wire type. A repeated numeric field
/// always accepts the LEN form (a packed run, whatever the declared form) and a
/// packed field also accepts its unpacked element form.
fn wt_incompatible(f: &Field, wt: u8) -> bool {
    if f.kind == Kind::Group {
        return wt != WT_START_GROUP;
    }
    if f.repeated && f.kind.is_packable() {
        if wt == WT_LEN {
            return false;
        }
        if f.packed {
            return wt != f.elem_wt;
        }
    }
    wt != f.wt
}

/// Stores a value in a slot, collapsing the oneof it belongs to.
fn store(
    schema: &CoreSchema,
    def: usize,
    v: &mut [Option<Value>],
    cases: &mut [u32],
    slot: usize,
    value: Value,
) {
    let oneof = schema.messages[def].fields[slot].oneof;
    if oneof >= 0 && !cases.is_empty() {
        let prev = cases[oneof as usize];
        if prev != 0 && prev != slot as u32 + 1 {
            v[prev as usize - 1] = None;
        }
        cases[oneof as usize] = slot as u32 + 1;
    }
    v[slot] = Some(value);
}

fn list_mut<'a>(v: &'a mut [Option<Value>], slot: usize) -> &'a mut Vec<Value> {
    if v[slot].is_none() {
        v[slot] = Some(Value::List(Vec::new()));
    }
    match &mut v[slot] {
        Some(Value::List(items)) => items,
        _ => unreachable!("slot {slot} is not a list"),
    }
}

pub(crate) fn decode_into(
    m: &mut Message,
    r: &mut Reader,
    end: usize,
    group_end: i64,
    depth: u32,
) -> Result<()> {
    if depth > r.cap {
        return fail_status(format!("nesting exceeds {}", r.cap), 4);
    }
    let outer_end = r.end;
    r.end = end;
    let mut closed = group_end < 0;
    {
        let Message { schema, def, v, oneof_case, unknown } = m;
        let def_idx = *def;
        while r.pos < r.end {
            let field_start = r.pos;
            let tag = r.r_tag()?;
            let fnum = tag >> 3;
            let wt = (tag & 7) as u8;
            if fnum == 0 || wt > WT_FIXED32 {
                return fail_status(format!("bad tag {tag}"), 1);
            }
            if wt == WT_END_GROUP {
                // Only our own number closes us; any other end tag is stray.
                if group_end < 0 || fnum as i64 != group_end {
                    return fail_status(format!("stray end-group for {fnum}"), 1);
                }
                closed = true;
                break;
            }
            let slot = schema.slot_of(def_idx, fnum);
            if slot < 0 || wt_incompatible(&schema.messages[def_idx].fields[slot as usize], wt) {
                // Wrong wire type (or unknown number) keeps the raw blob.
                r.skip_unknown(fnum, wt, depth)?;
                unknown.push(r.b[field_start..r.pos].to_vec());
                continue;
            }
            read_known(schema, def_idx, v, oneof_case, r, slot as usize, wt, depth)?;
        }
    }
    r.end = outer_end;
    if !closed {
        return fail_status(format!("missing end-group for {group_end}"), 1);
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn read_known(
    schema: &Rc<CoreSchema>,
    def: usize,
    v: &mut [Option<Value>],
    cases: &mut [u32],
    r: &mut Reader,
    slot: usize,
    wt: u8,
    depth: u32,
) -> Result<()> {
    let f = &schema.messages[def].fields[slot];
    let kind = f.kind;
    if f.repeated {
        if kind.is_packable() {
            let items = list_mut(v, slot);
            if wt == WT_LEN {
                read_packed(r, kind, items)?;
                if items.is_empty() {
                    // An empty packed run adds nothing: presence follows
                    // length, so drop the holder again.
                    v[slot] = None;
                }
                return Ok(());
            }
            let value = read_num_single(r, kind)?;
            items.push(value);
            return Ok(());
        }
        if kind == Kind::Message {
            let length = r.r_len32()?;
            let sub_end = r.pos + length;
            let mut sub = Message::new(Rc::clone(schema), f.msg_type.unwrap());
            decode_into(&mut sub, r, sub_end, -1, depth + 1)?;
            list_mut(v, slot).push(Value::Msg(Box::new(sub)));
            return Ok(());
        }
        if kind == Kind::Group {
            let mut sub = Message::new(Rc::clone(schema), f.msg_type.unwrap());
            let group_end = f.number;
            decode_into(&mut sub, r, r.end, group_end as i64, depth + 1)?;
            list_mut(v, slot).push(Value::Msg(Box::new(sub)));
            return Ok(());
        }
        // String / bytes element: validate, then record one payload.
        let length = r.r_len32()?;
        if f.utf8 && !utf8_valid(&r.b[r.pos..r.pos + length]) {
            return fail_status("invalid UTF-8 in a string field", 1);
        }
        let payload = r.b[r.pos..r.pos + length].to_vec();
        r.pos += length;
        list_mut(v, slot).push(Value::Bytes(payload));
        return Ok(());
    }
    if kind.is_packable() {
        let value = read_num_single(r, kind)?;
        // Implicit presence is decided on the *field's* zero, not the raw wire
        // bits: an int32/uint32 varint wider than 32 bits truncates and the
        // reference then drops the field as an implicit zero.
        if f.presence == Presence::Implicit && is_zero(&value) {
            return Ok(());
        }
        store(schema, def, v, cases, slot, value);
        return Ok(());
    }
    // Ref-kind singular: a second occurrence merges (like the core).
    if kind == Kind::Message {
        let length = r.r_len32()?;
        let sub_end = r.pos + length;
        let can_merge = match &v[slot] {
            Some(Value::Msg(_)) => {
                f.oneof < 0 || cases.is_empty() || cases[f.oneof as usize] == slot as u32 + 1
            }
            _ => false,
        };
        if can_merge {
            if let Some(Value::Msg(sub)) = &mut v[slot] {
                return decode_into(sub, r, sub_end, -1, depth + 1);
            }
        }
        let mut sub = Message::new(Rc::clone(schema), f.msg_type.unwrap());
        decode_into(&mut sub, r, sub_end, -1, depth + 1)?;
        store(schema, def, v, cases, slot, Value::Msg(Box::new(sub)));
        return Ok(());
    }
    if kind == Kind::Group {
        let group_end = f.number;
        let can_merge = match &v[slot] {
            Some(Value::Msg(_)) => {
                f.oneof < 0 || cases.is_empty() || cases[f.oneof as usize] == slot as u32 + 1
            }
            _ => false,
        };
        if can_merge {
            if let Some(Value::Msg(sub)) = &mut v[slot] {
                return decode_into(sub, r, r.end, group_end as i64, depth + 1);
            }
        }
        let mut sub = Message::new(Rc::clone(schema), f.msg_type.unwrap());
        decode_into(&mut sub, r, r.end, group_end as i64, depth + 1)?;
        store(schema, def, v, cases, slot, Value::Msg(Box::new(sub)));
        return Ok(());
    }
    // String / bytes (an empty implicit string stays absent).
    let length = r.r_len32()?;
    if length == 0 && f.presence == Presence::Implicit {
        return Ok(());
    }
    // proto3/editions strings are validated here, at parse, like the reference:
    // a malformed value fails the decode instead of surfacing as U+FFFD.
    // proto2 keeps the loose mapping.
    if f.utf8 && !utf8_valid(&r.b[r.pos..r.pos + length]) {
        return fail_status("invalid UTF-8 in a string field", 1);
    }
    let payload = r.b[r.pos..r.pos + length].to_vec();
    r.pos += length;
    store(schema, def, v, cases, slot, Value::Bytes(payload));
    Ok(())
}

fn is_zero(v: &Value) -> bool {
    match v {
        Value::I32(x) => *x == 0,
        Value::U32(x) => *x == 0,
        Value::I64(x) => *x == 0,
        Value::U64(x) => *x == 0,
        Value::Bool(b) => !*b,
        Value::F32(x) => *x == 0,
        Value::F64(x) => *x == 0,
        _ => false,
    }
}

/// One packed run of numeric elements into `items`.
fn read_packed(r: &mut Reader, kind: Kind, items: &mut Vec<Value>) -> Result<()> {
    let length = r.r_len32()?;
    let end = r.pos + length;
    match kind {
        Kind::Bool => {
            while r.pos < end {
                let v = r.r_var_bool()?;
                items.push(Value::Bool(v));
            }
        }
        Kind::Fixed32 | Kind::Sfixed32 | Kind::Float => {
            let count = length / 4;
            items.reserve(count);
            for _ in 0..count {
                let v = r.r_fixed32()?;
                items.push(match kind {
                    Kind::Sfixed32 => Value::I32(v as i32),
                    _ => Value::U32(v),
                });
            }
        }
        Kind::Fixed64 | Kind::Sfixed64 | Kind::Double => {
            let count = length / 8;
            items.reserve(count);
            for _ in 0..count {
                let v = r.r_fixed64()?;
                items.push(match kind {
                    Kind::Sfixed64 => Value::I64(v as i64),
                    _ => Value::U64(v),
                });
            }
        }
        _ => {
            while r.pos < end {
                let v = read_packed_var(r, kind)?;
                items.push(v);
            }
        }
    }
    if r.pos != end {
        return fail_status("packed overrun", 1);
    }
    Ok(())
}

fn read_packed_var(r: &mut Reader, kind: Kind) -> Result<Value> {
    Ok(match kind {
        Kind::Int32 | Kind::Enum => Value::I32(r.r_var32()? as i32),
        Kind::Uint32 => Value::U32(r.r_var32()?),
        // Truncate before unzigzag: a 64-bit varint on a sint32 field decodes
        // its low 32 bits, like the reference.
        Kind::Sint32 => Value::I32(unzig32(r.r_var32()?)),
        Kind::Int64 => Value::I64(r.r_var64()? as i64),
        Kind::Uint64 => Value::U64(r.r_var64()?),
        Kind::Sint64 => Value::I64(unzig64(r.r_var64()?)),
        _ => return fail_status("not numeric", 1),
    })
}

/// One numeric element in its storage form.
fn read_num_single(r: &mut Reader, kind: Kind) -> Result<Value> {
    Ok(match kind {
        Kind::Int32 | Kind::Enum => Value::I32(r.r_var32()? as i32),
        Kind::Uint32 => Value::U32(r.r_var32()?),
        Kind::Bool => Value::Bool(r.r_var_bool()?),
        Kind::Sint32 => Value::I32(unzig32(r.r_var32()?)),
        Kind::Int64 => Value::I64(r.r_var64()? as i64),
        Kind::Uint64 => Value::U64(r.r_var64()?),
        Kind::Sint64 => Value::I64(unzig64(r.r_var64()?)),
        Kind::Fixed32 => Value::U32(r.r_fixed32()?),
        Kind::Sfixed32 => Value::I32(r.r_fixed32()? as i32),
        Kind::Float => Value::F32(r.r_fixed32()?),
        Kind::Fixed64 => Value::U64(r.r_fixed64()?),
        Kind::Sfixed64 => Value::I64(r.r_fixed64()? as i64),
        Kind::Double => Value::F64(r.r_fixed64()?),
        _ => return fail_status("not numeric", 1),
    })
}

/// Whole-tree required check: every present nested message is checked too, so a
/// missing proto2 required field fails the decode, like the core.
fn check_required(m: &Message, cap: u32, depth: u32) -> Result<()> {
    if depth > cap {
        return fail_status(format!("nesting exceeds {cap}"), 4);
    }
    let def = m.def();
    if def.has_required {
        for (slot, f) in def.fields.iter().enumerate() {
            if f.presence == Presence::Required && m.v[slot].is_none() {
                return Err(CoreError::with_status(
                    format!("missing required field {}.{}", def.full_name, f.name),
                    6,
                ));
            }
        }
    }
    for (slot, f) in def.fields.iter().enumerate() {
        if !matches!(f.kind, Kind::Message | Kind::Group) {
            continue;
        }
        let Some(value) = &m.v[slot] else { continue };
        match value {
            Value::Msg(sub) => check_required(sub, cap, depth + 1)?,
            Value::List(items) => {
                for item in items {
                    if let Value::Msg(sub) = item {
                        check_required(sub, cap, depth + 1)?;
                    }
                }
            }
            _ => {}
        }
    }
    Ok(())
}

