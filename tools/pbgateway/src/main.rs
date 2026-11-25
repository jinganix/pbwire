//! pbgateway: fixed-path protobuf echo server.
//!
//! One route per RPC in the overlay descriptor set:
//! `POST /pb/<Fully.Qualified.Service>/<Method>` with a binary message body
//! answers `200` plus the re-encoded message (echo of the decoded tree, so
//! unknown fields pass through). `Accept: application/json` answers JSON
//! instead. Unknown paths answer `404` text, undecodable bodies `400` text.
//!
//! The descriptor set loads once at startup and every route resolves at
//! startup (fail fast on mismatch). Each request decodes and re-encodes with
//! a fresh message tree (the `pbwire` safe wrapper has no `pbw_ctx` pooling;
//! see the crate docs). Request bodies are capped at 4 MiB.
//!
//! The HTTP layer is `std` `TcpListener` only (thread per connection,
//! `Connection: close`): one route needs exact path matching, not a router.
//!
//! usage: `pbgateway --desc <file.desc> [--port 8081]`

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::ExitCode;
use std::sync::Arc;

/// 4 MiB guard on request bodies (and the codec-level decode bound).
const MAX_BYTES: usize = 4 * 1024 * 1024;
/// Cap on the request head (`\r\n\r\n` search space).
const MAX_HEAD: usize = 64 * 1024;

/// One fixed-path route: `POST /pb/<service>/<method>`.
struct Route {
    input: String,
    output: String,
}

fn usage() -> &'static str {
    "usage: pbgateway --desc <file.desc> [--port 8081]"
}

fn parse_args(argv: &[String]) -> Result<(String, u16), String> {
    let mut desc: Option<String> = None;
    let mut port: u16 = 8081;
    let mut it = argv.iter().peekable();
    while let Some(a) = it.next() {
        if a == "--desc" {
            match it.next() {
                Some(f) => desc = Some(f.clone()),
                None => return Err(format!("pbgateway: --desc needs a file argument\n{usage}", usage = usage())),
            }
        } else if let Some(f) = a.strip_prefix("--desc=") {
            desc = Some(f.to_string());
        } else if a == "--port" {
            match it.next() {
                Some(p) => {
                    port = p
                        .parse()
                        .map_err(|_| format!("pbgateway: bad --port '{p}'\n{}", usage()))?
                }
                None => return Err(format!("pbgateway: --port needs a number\n{usage}", usage = usage())),
            }
        } else if let Some(p) = a.strip_prefix("--port=") {
            port = p
                .parse()
                .map_err(|_| format!("pbgateway: bad --port '{p}'\n{}", usage()))?;
        } else {
            return Err(format!("pbgateway: unexpected argument '{a}'\n{}", usage()));
        }
    }
    match desc {
        Some(d) => Ok((d, port)),
        None => Err(format!("pbgateway: missing --desc <file.desc>\n{}", usage())),
    }
}

/// Loads `--desc` once and resolves every service/method at startup so a
/// descriptor mismatch fails fast instead of per request.
fn load_routes(desc_path: &str, schema: &pbwire::Schema) -> Result<HashMap<String, Route>, String> {
    let mut routes = HashMap::new();
    for i in 0..schema.service_count() {
        let Some(svc) = schema.service_at(i) else {
            continue;
        };
        let svc_name = svc.name().to_string();
        for j in 0..svc.method_count() {
            let Some(m) = svc.method_at(j) else {
                continue;
            };
            let meth_name = m.name().to_string();
            // Fail fast: the method must resolve to input/output types.
            let (input, output) = schema
                .resolve_method(&svc_name, &meth_name)
                .map_err(|e| format!("pbgateway: {desc_path}: {svc_name}/{meth_name}: {e}"))?;
            routes.insert(
                format!("/pb/{svc_name}/{meth_name}"),
                Route {
                    input: input.name().to_string(),
                    output: output.name().to_string(),
                },
            );
        }
    }
    if routes.is_empty() {
        return Err(format!("pbgateway: {desc_path}: no services in descriptor set"));
    }
    Ok(routes)
}

fn reason(code: u16) -> &'static str {
    match code {
        200 => "OK",
        400 => "Bad Request",
        404 => "Not Found",
        413 => "Payload Too Large",
        500 => "Internal Server Error",
        _ => "Error",
    }
}

fn respond(stream: &mut TcpStream, code: u16, content_type: &str, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {code} {}\r\ncontent-type: {content_type}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n",
        reason(code),
        body.len()
    );
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.write_all(body);
}

fn respond_text(stream: &mut TcpStream, code: u16, body: &str) {
    respond(stream, code, "text/plain; charset=utf-8", body.as_bytes());
}

/// Reads until `\r\n\r\n` (inclusive end index) or EOF/error.
fn read_head(stream: &mut TcpStream) -> Option<Vec<u8>> {
    let mut head = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        let n = stream.read(&mut buf).ok()?;
        if n == 0 {
            return None;
        }
        head.extend_from_slice(&buf[..n]);
        if head.len() > MAX_HEAD {
            return None;
        }
        if head.windows(4).any(|w| w == b"\r\n\r\n") {
            return Some(head);
        }
    }
}

fn handle(stream: &mut TcpStream, schema: &pbwire::Schema, routes: &HashMap<String, Route>) {
    let head = match read_head(stream) {
        Some(h) => h,
        None => return, // EOF / probe connection / oversize head: just close.
    };
    let sep = match head.windows(4).position(|w| w == b"\r\n\r\n") {
        Some(i) => i,
        None => return,
    };
    let head_text = match std::str::from_utf8(&head[..sep]) {
        Ok(t) => t,
        Err(_) => {
            respond_text(stream, 400, "bad request: headers are not UTF-8\n");
            return;
        }
    };
    let mut lines = head_text.lines();
    let request_line = lines.next().unwrap_or("");
    let mut parts = request_line.split_whitespace();
    let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
    let mut content_length: usize = 0;
    let mut accept_json = false;
    for line in lines {
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        match name.trim().to_ascii_lowercase().as_str() {
            "content-length" => match value.trim().parse() {
                Ok(n) => content_length = n,
                Err(_) => {
                    respond_text(stream, 400, "bad request: bad Content-Length\n");
                    return;
                }
            },
            "accept" => {
                if value.to_ascii_lowercase().contains("application/json") {
                    accept_json = true;
                }
            }
            _ => {}
        }
    }

    // Only POST routes exist; anything else is an unknown path.
    if method != "POST" {
        respond_text(stream, 404, "not found\n");
        return;
    }
    if content_length > MAX_BYTES {
        respond_text(stream, 413, "payload too large: 4 MiB cap\n");
        return;
    }
    // The head buffer may already hold part (or all) of the body.
    let mut body = head[sep + 4..].to_vec();
    body.resize(content_length, 0);
    let have = (head.len() - (sep + 4)).min(content_length);
    let mut read = have;
    while read < content_length {
        match stream.read(&mut body[read..content_length]) {
            Ok(0) => {
                respond_text(stream, 400, "bad request: truncated body\n");
                return;
            }
            Ok(n) => read += n,
            Err(_) => return,
        }
    }

    // Exact match on the path (query strings never route).
    let path = target.split('?').next().unwrap_or("");
    let Some(route) = routes.get(path) else {
        respond_text(stream, 404, "not found\n");
        return;
    };

    let input = match schema.message(&route.input) {
        Ok(t) => t,
        Err(e) => {
            respond_text(stream, 500, &format!("schema: {e}\n"));
            return;
        }
    };
    // One fresh tree per request: the safe wrapper has no pbw_ctx pooling.
    let msg = match input.decode_limits(&body, MAX_BYTES, 0) {
        Ok(m) => m,
        Err(e) => {
            respond_text(
                stream,
                400,
                &format!("failed to decode Protobuf message: {e}\n"),
            );
            return;
        }
    };
    // Echo: PlaceOrder takes Order and returns Order; the decoded tree is the
    // answer. Touch the output type so a route whose output vanished cannot
    // silently serve (startup resolve already guarantees this).
    debug_assert!(!route.output.is_empty());

    if accept_json {
        match msg.print_json() {
            Ok(json) => respond(stream, 200, "application/json", json.as_bytes()),
            Err(e) => respond_text(stream, 500, &format!("json print: {e}\n")),
        }
    } else {
        match msg.encode() {
            Ok(wire) => respond(stream, 200, "application/octet-stream", &wire),
            Err(e) => respond_text(stream, 500, &format!("encode: {e}\n")),
        }
    }
}

fn run(desc_path: &str, port: u16) -> Result<(), String> {
    // Descriptor bytes are read once; every worker below parses its own copy
    // because `pbwire::Schema` is `!Send` (one schema per thread, like one
    // message per thread in the C core's model).
    let desc_bytes = std::fs::read(desc_path)
        .map_err(|e| format!("pbgateway: {desc_path}: {e}"))?;
    // Fail fast in main: the whole route table must resolve before serving.
    let probe = pbwire::Schema::from_bytes(&desc_bytes)
        .map_err(|e| format!("pbgateway: {desc_path}: {e}"))?;
    let routes = load_routes(desc_path, &probe)?;
    drop(probe);
    let listener = TcpListener::bind(format!("127.0.0.1:{port}"))
        .map_err(|e| format!("pbgateway: bind 127.0.0.1:{port}: {e}"))?;
    eprintln!(
        "pbgateway: listening on 127.0.0.1:{port} ({} routes from {desc_path})",
        routes.len()
    );
    let routes = Arc::new(routes);
    let desc_bytes = Arc::new(desc_bytes);
    let (tx, rx) = std::sync::mpsc::channel::<TcpStream>();
    let rx = Arc::new(std::sync::Mutex::new(rx));
    // Fixed worker pool: one `Schema` per worker thread, loaded once here.
    let workers = std::thread::available_parallelism()
        .map(|n| n.get().min(16))
        .unwrap_or(4)
        .max(1);
    for _ in 0..workers {
        let rx = Arc::clone(&rx);
        let routes = Arc::clone(&routes);
        let desc_bytes = Arc::clone(&desc_bytes);
        std::thread::spawn(move || {
            let schema = pbwire::Schema::from_bytes(&desc_bytes)
                .expect("pbgateway: worker: descriptor set reload failed");
            loop {
                let stream = rx.lock().expect("queue").recv();
                let Ok(mut stream) = stream else { return };
                handle(&mut stream, &schema, &routes);
            }
        });
    }
    for conn in listener.incoming() {
        let Ok(stream) = conn else { continue };
        if tx.send(stream).is_err() {
            break;
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let (desc, port) = match parse_args(&argv) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };
    match run(&desc, port) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}
