//! Google conformance testee driven by the pure-Rust core: the same wire
//! protocol as the other bindings' testees (4-byte little-endian length prefix,
//! then a `ConformanceRequest` / `ConformanceResponse`) and the same
//! schema-driven shape -- the request names the message type and the descriptor
//! set answers with the schema, so there is no per-message code and no C
//! library.
//!
//! Binary in -> binary out is what this core implements. JSON and text requests
//! answer `skipped` (the runner does not count a skip as a failure), so the pass
//! set is the binary suite, not the JSON/text mappings.
//!
//!   cargo run --release --no-default-features --features core \
//!       --bin pbwire-conformance -- build/conformance.desc
//!
//! Driven by `make conformance-rust` from the repo root.

use std::io::{Read, Write};
use std::process::ExitCode;

use pbwire::core::{CoreError, Message, MessageType, Schema};

/// `WireFormat.PROTOBUF`; the only output this core renders.
const PROTOBUF: u64 = 1;

fn main() -> ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: pbwire-conformance <conformance.desc>");
        return ExitCode::from(2);
    };
    let schema = match Schema::from_file(&path) {
        Ok(schema) => schema,
        Err(e) => {
            eprintln!("load {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let request_ty = match schema.message("conformance.ConformanceRequest") {
        Ok(ty) => ty,
        Err(e) => {
            eprintln!("schema: {e}");
            return ExitCode::FAILURE;
        }
    };
    let response_ty = match schema.message("conformance.ConformanceResponse") {
        Ok(ty) => ty,
        Err(e) => {
            eprintln!("schema: {e}");
            return ExitCode::FAILURE;
        }
    };
    serve(&request_ty, &response_ty)
}

fn serve(request_ty: &MessageType, response_ty: &MessageType) -> ExitCode {
    let stdin = std::io::stdin();
    let stdout = std::io::stdout();
    let mut input = stdin.lock();
    let mut output = stdout.lock();
    loop {
        let Some(body) = read_frame(&mut input) else {
            return ExitCode::SUCCESS;
        };
        let response = handle(request_ty, response_ty, &body);
        let payload = match response_ty.encode(&response) {
            Ok(payload) => payload,
            Err(e) => {
                // A response we cannot encode is a bug in the testee, not in
                // the message under test: say so and stop.
                eprintln!("cannot encode ConformanceResponse: {e}");
                return ExitCode::FAILURE;
            }
        };
        if !write_frame(&mut output, &payload) {
            return ExitCode::SUCCESS;
        }
    }
}

/// Reads one length-prefixed request; `None` at end of input.
fn read_frame(input: &mut impl Read) -> Option<Vec<u8>> {
    let mut header = [0u8; 4];
    if input.read_exact(&mut header).is_err() {
        return None;
    }
    let size = u32::from_le_bytes(header) as usize;
    let mut body = vec![0u8; size];
    if input.read_exact(&mut body).is_err() {
        return None;
    }
    Some(body)
}

fn write_frame(output: &mut impl Write, payload: &[u8]) -> bool {
    let header = (payload.len() as u32).to_le_bytes();
    output.write_all(&header).is_ok()
        && output.write_all(payload).is_ok()
        && output.flush().is_ok()
}

/// One request in, one response out. Never fails: a failure is a response
/// field, so the runner reports it instead of the testee exiting.
fn handle(
    request_ty: &MessageType,
    response_ty: &MessageType,
    body: &[u8],
) -> Message {
    let mut response = response_ty.new_message();
    let request = match request_ty.decode(body) {
        Ok(request) => request,
        Err(e) => {
            set_string(&mut response, "runtime_error", &format!("cannot parse ConformanceRequest: {e}"));
            return response;
        }
    };
    let wanted = request.get_u64("requested_output_format").ok().flatten().unwrap_or(0);
    if wanted != PROTOBUF {
        set_string(&mut response, "skipped", &format!("output format {wanted} is not supported"));
        return response;
    }
    let has = |name: &str| request.has(name).unwrap_or(false);
    if has("json_payload") || has("text_payload") {
        set_string(
            &mut response,
            "skipped",
            "JSON and text input are not supported by the pure-Rust core",
        );
        return response;
    }
    if !has("protobuf_payload") {
        set_string(&mut response, "skipped", "no binary payload");
        return response;
    }
    let name = request.get_str("message_type").ok().flatten().unwrap_or_default();
    let ty = match request_ty.schema().message_index(&name) {
        Ok(idx) => MessageType::from_parts(request_ty.schema().clone(), idx),
        Err(_) => {
            set_string(&mut response, "runtime_error", &format!("unknown message type {name:?}"));
            return response;
        }
    };
    let payload = request.get_bytes("protobuf_payload").ok().flatten().unwrap_or(&[]);
    let message = match ty.decode(payload) {
        Ok(message) => message,
        Err(e) => {
            set_string(&mut response, "parse_error", &error_text(&e));
            return response;
        }
    };
    match ty.encode(&message) {
        Ok(bytes) => {
            // The response carries the bytes under `protobuf_payload`.
            if let Err(e) = response.set_bytes("protobuf_payload", &bytes) {
                set_string(&mut response, "serialize_error", &format!("{e}"));
            }
        }
        Err(e) => set_string(&mut response, "serialize_error", &error_text(&e)),
    }
    response
}

/// The runner only reads the text, so the status is not part of it: the message
/// alone is what the reference reports for a parse failure.
fn error_text(e: &CoreError) -> String {
    e.message().to_string()
}

fn set_string(response: &mut Message, field: &str, value: &str) {
    // A failure to set a response field means the descriptor is not
    // conformance.proto; the empty response then reaches the runner as a
    // missing result, which is the loudest signal available here.
    let _ = response.set_str(field, value);
}
