//! pbcurl: list and describe protobuf services from a descriptor set.
//!
//! v1 is offline only: `list` prints service names, `describe` prints one
//! service's methods with input/output types. Both load a
//! `FileDescriptorSet` (`protoc --descriptor_set_out --include_imports`)
//! through the pbwire Rust binding. `--proto` sources are rejected: compile
//! them to a descriptor set first (protoc step in README).
//!
//! The subcommand enum keeps room for a future `invoke` subcommand (Task 3).

use std::process::ExitCode;

/// list/describe share this: which descriptor set to load.
struct DescArgs {
    desc: String,
}

/// v1 subcommands. `Invoke` arrives in a later task; the enum (not a one-shot
/// main) keeps that extension point.
enum Subcommand {
    List(DescArgs),
    Describe { desc: DescArgs, service: String },
}

fn usage() -> &'static str {
    "usage:\n  pbcurl list --desc <file.desc>\n  pbcurl describe --desc <file.desc> <Service>"
}

/// Rejects `--proto` anywhere: v1 only reads descriptor sets.
fn check_no_proto(args: &[String]) -> Result<(), String> {
    if args.iter().any(|a| a == "--proto") {
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

fn run(argv: &[String]) -> Result<String, (String, u8)> {
    let cmd = parse(argv).map_err(|e| (e, 2))?;
    match cmd {
        Subcommand::List(desc) => cmd_list(&desc).map_err(|e| (e, 1)),
        Subcommand::Describe { desc, service } => {
            cmd_describe(&desc, &service).map_err(|e| (e, 1))
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
