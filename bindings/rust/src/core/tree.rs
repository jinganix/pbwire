//! The dynamic message tree: one slot per declared field, plus the oneof cases
//! and the retained unknown fields.
//!
//! Scalars live in their wire-neutral form (`i32`/`u32`/`i64`/`u64` for every
//! integer width, so nothing is truncated to what the host could hold; raw bit
//! patterns for `float`/`double` so a NaN payload survives a round trip;
//! `bool`; a byte vector for both `string` and `bytes`) and messages live
//! nested. A decoded string stays the exact bytes that came off the wire, so a
//! proto2 string with invalid UTF-8 re-encodes byte for byte.
//!
//! This is the Rust twin of `bindings/python/pure/tree.py`. Unlike the
//! interpreter cores it owns no slice of the input: strings and unknown fields
//! are copied out at parse, which keeps `Message` free of a lifetime and lets a
//! decoded tree outlive the buffer it came from.

use std::rc::Rc;

use super::descriptor::{CoreSchema, Field, Kind, MsgDef};
use super::wire::{fail, CoreError, Result};

/// One field value in its storage form.
#[derive(Debug, Clone)]
pub enum Value {
    Bool(bool),
    I32(i32),
    U32(u32),
    I64(i64),
    U64(u64),
    /// float32 bits, kept raw so a NaN payload survives.
    F32(u32),
    /// float64 bits, kept raw so a NaN payload survives.
    F64(u64),
    /// A `string` or `bytes` payload, exactly as parsed (or as set).
    Bytes(Vec<u8>),
    Msg(Box<Message>),
    List(Vec<Value>),
}

impl Value {
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::I32(v) => Some(*v as i64),
            Value::U32(v) => Some(*v as i64),
            Value::I64(v) => Some(*v),
            Value::U64(v) => Some(*v as i64),
            Value::Bool(v) => Some(*v as i64),
            Value::F32(v) => Some(*v as i32 as i64),
            Value::F64(v) => Some(*v as i64),
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Value::I32(v) => Some(*v as u64),
            Value::U32(v) => Some(*v as u64),
            Value::I64(v) => Some(*v as u64),
            Value::U64(v) => Some(*v),
            Value::Bool(v) => Some(*v as u64),
            Value::F32(v) => Some(*v as u64),
            Value::F64(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(v) => Some(*v),
            Value::I32(v) => Some(*v != 0),
            Value::U32(v) => Some(*v != 0),
            Value::I64(v) => Some(*v != 0),
            Value::U64(v) => Some(*v != 0),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::Bytes(b) => Some(b),
            _ => None,
        }
    }

    pub fn as_message(&self) -> Option<&Message> {
        match self {
            Value::Msg(m) => Some(m),
            _ => None,
        }
    }

    pub fn as_list(&self) -> Option<&[Value]> {
        match self {
            Value::List(items) => Some(items),
            _ => None,
        }
    }
}

/// One message value: per-slot values, the oneof cases and the unknowns.
#[derive(Debug, Clone)]
pub struct Message {
    pub(crate) schema: Rc<CoreSchema>,
    pub(crate) def: usize,
    pub(crate) v: Vec<Option<Value>>,
    /// Slot+1 per oneof, 0 = none set; empty when the message has none.
    pub(crate) oneof_case: Vec<u32>,
    /// Retained unknown fields in input order (tag and payload verbatim).
    pub(crate) unknown: Vec<Vec<u8>>,
}

impl Message {
    pub fn new(schema: Rc<CoreSchema>, def: usize) -> Message {
        let n = schema.messages[def].fields.len();
        let oneofs = schema.messages[def].oneof_count;
        Message {
            schema,
            def,
            v: vec![None; n],
            oneof_case: if oneofs > 0 { vec![0; oneofs] } else { Vec::new() },
            unknown: Vec::new(),
        }
    }

    pub fn full_name(&self) -> &str {
        &self.schema.messages[self.def].full_name
    }

    pub fn schema(&self) -> &Rc<CoreSchema> {
        &self.schema
    }

    pub fn def(&self) -> &MsgDef {
        &self.schema.messages[self.def]
    }

    pub fn schema_of(&self) -> &CoreSchema {
        &self.schema
    }

    /// Slot index of a field name.
    pub fn field_slot(&self, name: &str) -> Result<usize> {
        match self.def().by_name.get(name) {
            Some(slot) => Ok(*slot),
            None => fail(format!("{} has no field {name}", self.full_name())),
        }
    }

    pub fn field(&self, name: &str) -> Result<&Field> {
        let slot = self.field_slot(name)?;
        Ok(&self.def().fields[slot])
    }

    pub fn has(&self, name: &str) -> Result<bool> {
        let slot = self.field_slot(name)?;
        Ok(self.v[slot].is_some())
    }

    /// The raw slot value, which is `Some(List(..))` for a repeated field.
    pub fn get(&self, name: &str) -> Result<Option<&Value>> {
        let slot = self.field_slot(name)?;
        Ok(self.v[slot].as_ref())
    }

    /// Which member of a oneof is set: its field number, or 0 for none.
    pub fn oneof_case(&self, oneof_name: &str) -> Result<u32> {
        let idx = match self.def().oneof_by_name.get(oneof_name) {
            Some(i) => *i,
            None => return fail(format!("{} has no oneof {oneof_name}", self.full_name())),
        };
        let case = self.oneof_case.get(idx).copied().unwrap_or(0);
        Ok(if case == 0 { 0 } else { self.def().fields[case as usize - 1].number })
    }

    pub(crate) fn set_slot(&mut self, slot: usize, value: Value) {
        let oneof = self.schema.messages[self.def].fields[slot].oneof;
        if oneof >= 0 && !self.oneof_case.is_empty() {
            let prev = self.oneof_case[oneof as usize];
            if prev != 0 && prev != slot as u32 + 1 {
                self.v[prev as usize - 1] = None;
            }
            self.oneof_case[oneof as usize] = slot as u32 + 1;
        }
        self.v[slot] = Some(value);
    }

    /// Stores a value, converting it to the field's storage form.
    pub fn set(&mut self, name: &str, value: Value) -> Result<()> {
        let slot = self.field_slot(name)?;
        let f = &self.def().fields[slot];
        let value = coerce(f, value);
        self.set_slot(slot, value);
        Ok(())
    }

    /// Appends one element of a repeated field.
    pub fn push(&mut self, name: &str, value: Value) -> Result<()> {
        let slot = self.field_slot(name)?;
        let f = &self.def().fields[slot];
        let value = coerce(f, value);
        match &mut self.v[slot] {
            Some(Value::List(items)) => items.push(value),
            _ => {
                self.set_slot(slot, Value::List(vec![value]));
                return Ok(());
            }
        }
        Ok(())
    }

    pub fn set_i64(&mut self, name: &str, v: i64) -> Result<()> {
        self.set(name, Value::I64(v))
    }

    pub fn set_u64(&mut self, name: &str, v: u64) -> Result<()> {
        self.set(name, Value::U64(v))
    }

    pub fn set_bool(&mut self, name: &str, v: bool) -> Result<()> {
        self.set(name, Value::Bool(v))
    }

    pub fn set_f64(&mut self, name: &str, v: f64) -> Result<()> {
        self.set(name, Value::F64(v.to_bits()))
    }

    pub fn set_f32(&mut self, name: &str, v: f32) -> Result<()> {
        self.set(name, Value::F32(v.to_bits()))
    }

    pub fn set_str(&mut self, name: &str, v: &str) -> Result<()> {
        self.set(name, Value::Bytes(v.as_bytes().to_vec()))
    }

    pub fn set_bytes(&mut self, name: &str, v: &[u8]) -> Result<()> {
        self.set(name, Value::Bytes(v.to_vec()))
    }

    /// Replaces (or creates) a sub-message field and returns it for filling.
    pub fn set_message(&mut self, name: &str) -> Result<&mut Message> {
        let slot = self.field_slot(name)?;
        let msg_type = self.def().fields[slot]
            .msg_type
            .ok_or_else(|| CoreError::new(format!("{name} is not a message field")))?;
        let sub = Message::new(Rc::clone(&self.schema), msg_type);
        self.set_slot(slot, Value::Msg(Box::new(sub)));
        match &mut self.v[slot] {
            Some(Value::Msg(m)) => Ok(m),
            _ => fail("unreachable: set_message lost its value"),
        }
    }

    /// Creates a new element of a repeated message field and returns it.
    pub fn push_message(&mut self, name: &str) -> Result<&mut Message> {
        let slot = self.field_slot(name)?;
        let msg_type = self.def().fields[slot]
            .msg_type
            .ok_or_else(|| CoreError::new(format!("{name} is not a message field")))?;
        let sub = Value::Msg(Box::new(Message::new(Rc::clone(&self.schema), msg_type)));
        match &mut self.v[slot] {
            Some(Value::List(items)) => items.push(sub),
            _ => self.set_slot(slot, Value::List(vec![sub])),
        }
        match &mut self.v[slot] {
            Some(Value::List(items)) => match items.last_mut() {
                Some(Value::Msg(m)) => Ok(m),
                _ => fail("unreachable: push_message lost its value"),
            },
            _ => fail("unreachable: push_message lost its list"),
        }
    }

    pub fn get_i64(&self, name: &str) -> Result<Option<i64>> {
        Ok(self.get(name)?.and_then(|v| v.as_i64()))
    }

    pub fn get_u64(&self, name: &str) -> Result<Option<u64>> {
        Ok(self.get(name)?.and_then(|v| v.as_u64()))
    }

    pub fn get_bool(&self, name: &str) -> Result<Option<bool>> {
        Ok(self.get(name)?.and_then(|v| v.as_bool()))
    }

    pub fn get_f64(&self, name: &str) -> Result<Option<f64>> {
        Ok(self.get(name)?.and_then(|v| match v {
            Value::F64(bits) => Some(f64::from_bits(*bits)),
            Value::F32(bits) => Some(f32::from_bits(*bits) as f64),
            _ => None,
        }))
    }

    pub fn get_f32(&self, name: &str) -> Result<Option<f32>> {
        Ok(self.get(name)?.and_then(|v| match v {
            Value::F32(bits) => Some(f32::from_bits(*bits)),
            Value::F64(bits) => Some(f64::from_bits(*bits) as f32),
            _ => None,
        }))
    }

    pub fn get_bytes(&self, name: &str) -> Result<Option<&[u8]>> {
        Ok(self.get(name)?.and_then(|v| v.as_bytes()))
    }

    /// A string field's bytes as UTF-8 (lossy: proto2 strings may be invalid).
    pub fn get_str(&self, name: &str) -> Result<Option<String>> {
        match self.get_bytes(name)? {
            Some(b) => Ok(Some(String::from_utf8_lossy(b).into_owned())),
            None => Ok(None),
        }
    }

    pub fn get_message(&self, name: &str) -> Result<Option<&Message>> {
        Ok(self.get(name)?.and_then(|v| v.as_message()))
    }

    /// Number of elements in a repeated field.
    pub fn len(&self, name: &str) -> Result<usize> {
        Ok(match self.get(name)? {
            Some(Value::List(items)) => items.len(),
            _ => 0,
        })
    }

    pub fn is_empty(&self, name: &str) -> Result<bool> {
        Ok(self.len(name)? == 0)
    }

    /// The elements of a repeated field.
    pub fn items(&self, name: &str) -> Result<&[Value]> {
        Ok(match self.get(name)? {
            Some(Value::List(items)) => items.as_slice(),
            _ => &[],
        })
    }

    /// The retained unknown field images, in input order.
    pub fn unknown(&self) -> &[Vec<u8>] {
        &self.unknown
    }
}

/// One public value in the storage form of its field (see [`Kind`]).
fn coerce(f: &Field, v: Value) -> Value {
    match f.kind {
        Kind::Bool => Value::Bool(match &v {
            Value::Bool(b) => *b,
            other => other.as_i64().map_or(false, |i| i != 0),
        }),
        Kind::Int32 | Kind::Sint32 | Kind::Sfixed32 | Kind::Enum => {
            Value::I32(match &v {
                Value::I32(i) => *i,
                Value::U32(u) => *u as i32,
                Value::I64(i) => *i as i32,
                Value::U64(u) => *u as i32,
                Value::Bool(b) => *b as i32,
                Value::F32(bits) => *bits as i32,
                Value::F64(bits) => *bits as i32,
                _ => 0,
            })
        }
        Kind::Uint32 | Kind::Fixed32 => Value::U32(match &v {
            Value::I32(i) => *i as u32,
            Value::U32(u) => *u,
            Value::I64(i) => *i as u32,
            Value::U64(u) => *u as u32,
            Value::Bool(b) => *b as u32,
            Value::F32(bits) => *bits,
            Value::F64(bits) => *bits as u32,
            _ => 0,
        }),
        Kind::Int64 | Kind::Sint64 | Kind::Sfixed64 => Value::I64(match &v {
            Value::I32(i) => *i as i64,
            Value::U32(u) => *u as i64,
            Value::I64(i) => *i,
            Value::U64(u) => *u as i64,
            Value::Bool(b) => *b as i64,
            Value::F32(bits) => *bits as i32 as i64,
            Value::F64(bits) => *bits as i64,
            _ => 0,
        }),
        Kind::Uint64 | Kind::Fixed64 => Value::U64(match &v {
            Value::I32(i) => *i as u64,
            Value::U32(u) => *u as u64,
            Value::I64(i) => *i as u64,
            Value::U64(u) => *u,
            Value::Bool(b) => *b as u64,
            Value::F32(bits) => *bits as u64,
            Value::F64(bits) => *bits,
            _ => 0,
        }),
        Kind::Float => Value::F32(match &v {
            Value::F32(bits) => *bits,
            Value::F64(bits) => f64::from_bits(*bits) as f32 as u32,
            other => other.as_i64().unwrap_or(0) as f32 as u32,
        }),
        Kind::Double => Value::F64(match &v {
            Value::F64(bits) => *bits,
            Value::F32(bits) => f32::from_bits(*bits) as f64 as u64,
            other => other.as_i64().unwrap_or(0) as f64 as u64,
        }),
        Kind::String | Kind::Bytes => match v {
            Value::Bytes(b) => Value::Bytes(b),
            _ => Value::Bytes(Vec::new()),
        },
        Kind::Message | Kind::Group => v,
    }
}
