//! The `core` arm of the Rust pbench: the pure-Rust wire core
//! (`bindings/rust/src/core/`, the `core` cargo feature) as the codec, no C
//! library anywhere in the path.
//!
//! Same user-visible task as the other two arms: the payload is a native object
//! (here the core's own `Message` tree, built through the same per-field
//! `set_*` calls the `tree` arm makes), a prebuilt tree in and wire bytes out,
//! wire bytes in and a fully readable tree out. Build time is outside the
//! timer, exactly like `pbwire-rust-tree`'s prebuilt handle, so the two rows
//! differ only in who walks the wire.
//!
//! Included by `benchmarks/src/main.rs` (`#[path = "../pbench/core_arm.rs"]`),
//! so the arm is one file with no crate of its own.

use pbwire::core::{CoreError, Limits, Message, MessageType, Schema};

/// The one fixture every arm uses, wired to the core's own tree API.
pub struct CoreArm {
    scalars: MessageType,
}

impl CoreArm {
    pub fn load(descriptor_set: &str) -> Result<CoreArm, String> {
        let schema = Schema::from_file(descriptor_set).map_err(|e| e.to_string())?;
        let scalars = schema
            .message("structtest.Scalars3")
            .map_err(|e| e.to_string())?;
        // The handles hold their own share of the compiled schema, so the
        // `Schema` itself does not need to be kept.
        Ok(CoreArm { scalars })
    }

    /// The `matrix3` fixture through the core tree: every scalar kind at its
    /// edge, the nested message, the two proto3 optionals and the repeated
    /// fields scaled by `n`.
    pub fn build(&self, n: usize) -> Message {
        let mut m = self.scalars.new_message();
        let set = |m: &mut Message, name: &str, v: i64| {
            m.set_i64(name, v).expect("core: set_i64")
        };
        set(&mut m, "i32", i32::MIN as i64);
        set(&mut m, "i64", i64::MAX);
        m.set_u64("u32", u32::MAX as u64).expect("set_u64");
        m.set_u64("u64", u64::MAX).expect("set_u64");
        set(&mut m, "s32", i32::MIN as i64);
        set(&mut m, "s64", i64::MIN);
        m.set_u64("fx32", 0xdead_beef).expect("set_u64");
        m.set_u64("fx64", 72623859790382856).expect("set_u64");
        set(&mut m, "sfx32", i32::MIN as i64);
        set(&mut m, "sfx64", i64::MIN);
        m.set_f32("f32", f32::INFINITY).expect("set_f32");
        m.set_f64("f64", -123456.789).expect("set_f64");
        m.set_bool("flag", true).expect("set_bool");
        m.set_str("str", "hello").expect("set_str");
        set(&mut m, "color", 2);
        set(&mut m, "opt_i32", -7);
        m.set_str("opt_str", "opt").expect("set_str");
        {
            let sub = m.set_message("inner").expect("set_message");
            sub.set_str("name", "kid").expect("set_str");
            sub.set_i64("value", 42).expect("set_i64");
        }
        for i in 0..n {
            m.push("nums", pbwire::core::Value::I32(i as i32 - 3))
                .expect("push nums");
            m.push("tags", pbwire::core::Value::Bytes(b"tag".to_vec()))
                .expect("push tags");
            m.push("flags", pbwire::core::Value::Bool(i % 2 == 0))
                .expect("push flags");
            let sub = m.push_message("inners").expect("push inners");
            sub.set_str("name", "grandkid").expect("set_str");
            sub.set_i64("value", i as i64 + 1).expect("set_i64");
        }
        m
    }

    /// One encode into a reused buffer: the hot-loop shape every arm uses.
    pub fn encode_into(&self, m: &Message, buf: &mut Vec<u8>) -> usize {
        self.scalars.encode_to(m, buf).expect("core: encode");
        buf.len()
    }

    /// One-shot decode, like the other arms' decode rows.
    pub fn decode(&self, wire: &[u8]) -> Result<Message, CoreError> {
        self.scalars.decode(wire)
    }

    /// The gate: byte equality with the official runtime's reference image, and
    /// an oracle read of the decoded tree. Runs before any timing.
    pub fn gate(&self, reference: &[u8], n: usize) -> Result<(), String> {
        let message = self.build(n);
        let bytes = self.scalars.encode(&message).map_err(|e| e.to_string())?;
        if bytes != reference {
            return Err(format!(
                "pbwire-rust-core wrote {} bytes, protobuf-rust {}",
                bytes.len(),
                reference.len()
            ));
        }
        let back = self.decode(reference).map_err(|e| e.to_string())?;
        let re = self.scalars.encode(&back).map_err(|e| e.to_string())?;
        if re != reference {
            return Err("pbwire-rust-core re-encode differs".to_string());
        }
        if back.get_str("str").ok().flatten().as_deref() != Some("hello") {
            return Err("pbwire-rust-core decode: str".to_string());
        }
        if back.get_u64("u64").ok().flatten() != Some(u64::MAX) {
            return Err("pbwire-rust-core decode: u64".to_string());
        }
        if back.get_i64("opt_i32").ok().flatten() != Some(-7) {
            return Err("pbwire-rust-core decode: opt_i32".to_string());
        }
        if !back.has("opt_str").unwrap_or(false) {
            return Err("pbwire-rust-core decode: opt_str presence".to_string());
        }
        let inner_name = back
            .get_message("inner")
            .ok()
            .flatten()
            .and_then(|m| m.get_str("name").ok().flatten());
        if inner_name.as_deref() != Some("kid") {
            return Err("pbwire-rust-core decode: inner.name".to_string());
        }
        if back.len("nums").unwrap_or(0) != n {
            return Err("pbwire-rust-core decode: nums count".to_string());
        }
        let last = back
            .items("nums")
            .ok()
            .and_then(|items| items.last().and_then(|v| v.as_i64()));
        if n > 0 && last != Some(n as i64 - 4) {
            return Err("pbwire-rust-core decode: last num".to_string());
        }
        if back.len("inners").unwrap_or(0) != n {
            return Err("pbwire-rust-core decode: inners count".to_string());
        }
        let limits = Limits::default();
        self.scalars
            .decode_slice(reference, 0, reference.len(), &limits)
            .map_err(|e| e.to_string())?;
        Ok(())
    }
}
