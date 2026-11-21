//! Tests for the pure-Rust core (`bindings/rust/src/core/`), behind the `core`
//! cargo feature. Plain `#[test]` functions and explicit assertions -- no test
//! framework -- so the same files run standalone (`cargo test --features core
//! --test core`) and through the workspace default (`make rust-test`, where the
//! crate is empty without the feature).
//!
//!   cargo test --no-default-features --features core --test core
#![cfg(feature = "core")]

mod codec;
mod descriptor;
mod wire;
