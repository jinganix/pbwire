//! pbcurl: list/describe/invoke protobuf services via a descriptor set.
//!
//! `list` prints service names, `describe` prints one service's methods
//! with input/output types. Both load a `FileDescriptorSet`
//! (`protoc --descriptor_set_out --include_imports`) through the pbwire
//! Rust binding. `--proto` sources are rejected: compile them to a
//! descriptor set first (protoc step in README).
//!
//! `invoke` maps JSON to binary, POSTs it to `$PBURL_BASE/pb/<S>/<M>`, and
//! prints the JSON answer: `pbcurl invoke --desc <f> <S>/<M> '{"f":1}'`.
//! The HTTP layer is `std` `TcpStream` only (same style as the gateway):
//! one fixed-path POST needs no client crate.

use std::process::ExitCode;

/// list/describe share this: which descriptor set to load.
struct DescArgs {
    desc: String,
}

/// v1 subcommands; `Invoke` POSTs binary to the fixed-path gateway and
/// prints the JSON answer. New subcommands extend this enum.
enum Subcommand {
    List(DescArgs),
    Describe { desc: DescArgs, service: String },
    Invoke {
        desc: DescArgs,
        target: String,
        json: String,
    },
}

fn usage() -> &'static str {
    "usage:\n  pbcurl list --desc <file.desc>\n  pbcurl describe --desc <file.desc> <Service>\n  pbcurl invoke --desc <file.desc> <Service>/<Method> '<json>' [--json-out]"
}

/// Rejects `--proto` in any form (`--proto`, `--proto=...`, `--proto...`):
/// pbcurl only reads descriptor sets.
fn check_no_proto(args: &[String]) -> Result<(), String> {
    if args.iter().any(|a| a.starts_with("--proto")) {
        return Err("pbcurl: --proto is not supported in v1: \
            use --desc (protoc step in README)"
            .to_string());
    }
    Ok(())
}

/// Pulls `--desc <file>` out of `args`, returning the rest (positionals).
fn split_desc(args: &[String]) -> Result<(DescArgs, Vec<String>), String> {
    let mut desc: Option<String> = None;
    let mut rest = Vec::new();
    let mut it = args.iter().peekable();
    while let Some(a) = it.next() {
        if a == "--desc" {
            match it.next() {
                Some(f) => desc = Some(f.clone()),
                None => return Err("pbcurl: --desc needs a file argument\n".to_string() + usage()),
            }
        } else if let Some(f) = a.strip_prefix("--desc=") {
            desc = Some(f.to_string());
        } else {
            rest.push(a.clone());
        }
    }
    match desc {
        Some(d) => Ok((DescArgs { desc: d }, rest)),
        None => Err("pbcurl: missing --desc <file.desc>\n".to_string() + usage()),
    }
}

fn parse(args: &[String]) -> Result<Subcommand, String> {
    check_no_proto(args)?;
    let (sub, rest) = match args.split_first() {
        Some((head, tail)) => (head.as_str(), tail),
        None => return Err("pbcurl: missing subcommand\n".to_string() + usage()),
    };
    match sub {
        "list" => {
            let (desc, rest) = split_desc(rest)?;
            if !rest.is_empty() {
                return Err(format!(
                    "pbcurl: unexpected argument '{}'\n{}",
                    rest[0],
                    usage()
                ));
            }
            Ok(Subcommand::List(desc))
        }
        "describe" => {
            let (desc, rest) = split_desc(rest)?;
            match rest.as_slice() {
                [service] => Ok(Subcommand::Describe {
                    desc,
                    service: service.clone(),
                }),
                [] => Err("pbcurl: describe needs a <Service> argument\n".to_string() + usage()),
                _ => Err(format!(
                    "pbcurl: describe takes exactly one <Service> argument\n{}",
                    usage()
                )),
            }
        }
        other if other == "invoke" => {
            let (desc, rest) = split_desc(rest)?;
            // `--json-out` is accepted anywhere among the positionals for
            // script compat; JSON is already the only invoke output.
            let rest: Vec<String> =
                rest.into_iter().filter(|a| a != "--json-out").collect();
            match rest.as_slice() {
                [target, json] => Ok(Subcommand::Invoke {
                    desc,
                    target: target.clone(),
                    json: json.clone(),
                }),
                _ => Err(format!(
                    "pbcurl: invoke needs <Service>/<Method> and '<json>' arguments\n{}",
                    usage()
                )),
            }
        }
        other => Err(format!("pbcurl: unknown subcommand '{other}'\n{}", usage())),
    }
}

fn cmd_list(desc: &DescArgs) -> Result<String, String> {
    let schema = pbwire::Schema::from_file(&desc.desc).map_err(|e| format!("pbcurl: {e}"))?;
    let mut out = String::new();
    for i in 0..schema.service_count() {
        if let Some(svc) = schema.service_at(i) {
            out.push_str(svc.name());
            out.push('\n');
        }
    }
    Ok(out)
}

fn cmd_describe(desc: &DescArgs, service: &str) -> Result<String, String> {
    let schema = pbwire::Schema::from_file(&desc.desc).map_err(|e| format!("pbcurl: {e}"))?;
    let svc =
        schema
            .service(service)
            .map_err(|e| format!("pbcurl: {}: {e}", desc.desc))?;
    let mut out = String::new();
    for i in 0..svc.method_count() {
        if let Some(m) = svc.method_at(i) {
            out.push_str(&format!(
                "rpc {}({}) returns ({})\n",
                m.name(),
                m.input_type(),
                m.output_type()
            ));
        }
    }
    Ok(out)
}

/// Minimal `http://` POST over `std` `TcpStream`: one fixed path, one
/// binary body, full response read until close. Returns (status, body).
fn http_post(base: &str, path: &str, body: &[u8]) -> Result<(u16, Vec<u8>), String> {
    let auth = base.strip_prefix("http://").ok_or_else(|| {
        format!("pbcurl: PBURL_BASE must start with http://: '{base}'")
    })?;
    let (authority, prefix) = match auth.find('/') {
        Some(i) => (&auth[..i], auth[i..].trim_end_matches('/')),
        None => (auth, ""),
    };
    if authority.is_empty() {
        return Err(format!("pbcurl: bad PBURL_BASE: '{base}'"));
    }
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) => {
            let port: u16 = p
                .parse()
                .map_err(|_| format!("pbcurl: bad PBURL_BASE port: '{base}'"))?;
            (h, port)
        }
        None => (authority, 80),
    };
    if host.is_empty() {
        return Err(format!("pbcurl: bad PBURL_BASE: '{base}'"));
    }
    let target = format!("{prefix}{path}");
    let mut stream = std::net::TcpStream::connect((host, port))
        .map_err(|e| format!("pbcurl: connect {host}:{port}: {e}"))?;
    let head = format!(
        "POST {target} HTTP/1.1\r\n\
         host: {authority}\r\n\
         content-type: application/octet-stream\r\n\
         accept: application/json\r\n\
         content-length: {}\r\n\
         connection: close\r\n\r\n",
        body.len()
    );
    use std::io::{Read, Write};
    stream
        .write_all(head.as_bytes())
        .and_then(|()| stream.write_all(body))
        .map_err(|e| format!("pbcurl: POST {target}: {e}"))?;
    let mut resp = Vec::new();
    stream
        .read_to_end(&mut resp)
        .map_err(|e| format!("pbcurl: read response: {e}"))?;
    let sep = resp
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or_else(|| "pbcurl: bad response: no header terminator".to_string())?;
    let head_text = std::str::from_utf8(&resp[..sep])
        .map_err(|_| "pbcurl: bad response: headers are not UTF-8".to_string())?;
    let status: u16 = head_text
        .lines()
        .next()
        .unwrap_or("")
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .ok_or_else(|| "pbcurl: bad response: no status code".to_string())?;
    Ok((status, resp[sep + 4..].to_vec()))
}

fn cmd_invoke(desc: &DescArgs, target: &str, json: &str) -> Result<String, String> {
    let (service, method) = target.split_once('/').ok_or_else(|| {
        format!("pbcurl: invoke target must be <Service>/<Method>, got '{target}'\n{usage}", usage = usage())
    })?;
    let schema = pbwire::Schema::from_file(&desc.desc).map_err(|e| format!("pbcurl: {e}"))?;
    let (input, output) = schema
        .resolve_method(service, method)
        .map_err(|e| format!("pbcurl: {target}: {e}"))?;
    // JSON mapping -> binary request body.
    let msg = input
        .parse_json(json, false)
        .map_err(|e| format!("pbcurl: bad request JSON: {e}"))?;
    let wire = msg.encode().map_err(|e| format!("pbcurl: encode: {e}"))?;
    let base = std::env::var("PBURL_BASE")
        .unwrap_or_else(|_| "http://127.0.0.1:8081".to_string());
    let path = format!("/pb/{service}/{method}");
    let (status, body) =
        http_post(base.trim_end_matches('/'), &path, &wire)?;
    eprintln!("pbcurl: POST {base}{path} ({} bytes) -> {status}", wire.len());
    if status != 200 {
        let text = String::from_utf8_lossy(&body);
        return Err(format!("pbcurl: {path}: HTTP {status}: {text}"));
    }
    // The gateway answers JSON (`Accept: application/json`); validate it
    // against the output type and print the canonical mapping.
    let text = String::from_utf8(body)
        .map_err(|_| "pbcurl: response is not UTF-8 JSON".to_string())?;
    let out_msg = output
        .parse_json(&text, false)
        .map_err(|e| format!("pbcurl: bad response JSON: {e}"))?;
    let mut json = out_msg
        .print_json()
        .map_err(|e| format!("pbcurl: json print: {e}"))?;
    json.push('\n');
    Ok(json)
}

fn run(argv: &[String]) -> Result<String, (String, u8)> {
    let cmd = parse(argv).map_err(|e| (e, 2))?;
    match cmd {
        Subcommand::List(desc) => cmd_list(&desc).map_err(|e| (e, 1)),
        Subcommand::Describe { desc, service } => {
            cmd_describe(&desc, &service).map_err(|e| (e, 1))
        }
        Subcommand::Invoke { desc, target, json } => {
            cmd_invoke(&desc, &target, &json).map_err(|e| (e, 1))
        }
    }
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    match run(&argv) {
        Ok(text) => {
            print!("{text}");
            ExitCode::SUCCESS
        }
        Err((msg, code)) => {
            eprintln!("{msg}");
            ExitCode::from(code)
        }
    }
}
