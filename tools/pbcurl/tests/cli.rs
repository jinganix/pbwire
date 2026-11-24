use std::path::PathBuf;
use std::process::{Command, Output};

const GATEWAY_DESC: &str = "../pbnginx/gateway_svc.desc";

fn pbcurl_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_pbcurl"))
}

fn run_pbcurl(args: &[&str]) -> Output {
    Command::new(pbcurl_bin())
        .args(args)
        .output()
        .expect("spawn pbcurl")
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

#[test]
fn list_shows_gateway_service() {
    let out = run_pbcurl(&["list", "--desc", GATEWAY_DESC]);
    assert!(out.status.success());
    assert!(stdout(&out).contains("shop.OrderService"));
}

#[test]
fn describe_shows_placeorder_signature() {
    let out = run_pbcurl(&["describe", "--desc", GATEWAY_DESC, "shop.OrderService"]);
    assert!(out.status.success());
    let text = stdout(&out);
    assert!(text.contains("PlaceOrder"), "missing method:\n{text}");
    assert!(text.contains("shop.Order"), "missing input/output type:\n{text}");
}

#[test]
fn bad_desc_fails_with_stderr() {
    let out = run_pbcurl(&["list", "--desc", "/nonexistent/missing.desc"]);
    assert!(!out.status.success());
    assert!(!stderr(&out).trim().is_empty());
}

#[test]
fn proto_flag_rejected_with_desc_hint() {
    let out = run_pbcurl(&["list", "--proto", "foo.proto"]);
    assert!(!out.status.success());
    assert!(stderr(&out).contains("use --desc (protoc step in README)"));
}

#[test]
fn describe_unknown_service_fails_with_stderr() {
    let out = run_pbcurl(&["describe", "--desc", GATEWAY_DESC, "shop.NoSuch"]);
    assert!(!out.status.success());
    assert!(!stderr(&out).trim().is_empty());
}
