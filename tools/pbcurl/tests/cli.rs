use std::path::PathBuf;
use std::process::{Child, Command, Output};

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

fn run_pbcurl_with_env(args: &[&str], envs: &[(&str, &str)]) -> Output {
    let mut cmd = Command::new(pbcurl_bin());
    cmd.args(args);
    for (k, v) in envs {
        cmd.env(k, v);
    }
    cmd.output().expect("spawn pbcurl")
}

/// Test gateway port: the gateway only binds a fixed `--port`, so the test
/// uses 18081 (not the default 8081 a dev gateway may occupy).
const TEST_PORT: u16 = 18081;

fn gateway_bin() -> PathBuf {
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.push("../pbgateway/target/debug/pbgateway");
    p
}

/// Builds (once) and spawns the real `pbgateway` echo server, waiting until
/// it accepts connections. The caller must `kill()` the child.
fn start_gateway() -> Child {
    let bin = gateway_bin();
    if !bin.exists() {
        let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../pbgateway/Cargo.toml");
        let st = Command::new("cargo")
            .args(["build", "--manifest-path"])
            .arg(&manifest)
            .status()
            .expect("cargo build pbgateway");
        assert!(st.success(), "cargo build pbgateway failed");
    }
    let mut child = Command::new(&bin)
        .args([
            "--desc",
            GATEWAY_DESC,
            "--port",
            &TEST_PORT.to_string(),
        ])
        .spawn()
        .expect("spawn pbgateway");
    // Poll until the listener is up (spawn is async; the first dial races).
    for _ in 0..100 {
        if std::net::TcpStream::connect(("127.0.0.1", TEST_PORT)).is_ok() {
            return child;
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    let _ = child.kill();
    panic!("pbgateway did not come up on 127.0.0.1:{TEST_PORT}");
}

#[test]
fn invoke_echo_roundtrip() {
    let mut gw = start_gateway();
    let base = format!("http://127.0.0.1:{TEST_PORT}");
    let out = run_pbcurl_with_env(
        &[
            "invoke",
            "--desc",
            GATEWAY_DESC,
            "shop.OrderService/PlaceOrder",
            r#"{"id":"o1"}"#,
        ],
        &[("PBURL_BASE", base.as_str())],
    );
    let _ = gw.kill();
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    assert!(stdout(&out).contains(r#""id""#), "stdout: {}", stdout(&out));
    assert!(
        stdout(&out).contains("o1"),
        "echoed id missing: {}",
        stdout(&out)
    );
}

#[test]
fn invoke_unknown_method_fails() {
    let out = run_pbcurl(&[
        "invoke",
        "--desc",
        GATEWAY_DESC,
        "shop.OrderService/NoSuch",
        r#"{"id":"o1"}"#,
    ]);
    assert!(!out.status.success());
    assert!(!stderr(&out).trim().is_empty());
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
fn proto_equals_form_rejected_with_desc_hint() {
    for args in [
        vec!["list", "--proto=foo.proto"],
        vec!["list", "--protofoo"],
    ] {
        let out = run_pbcurl(&args);
        assert!(!out.status.success(), "args accepted: {args:?}");
        assert!(
            stderr(&out).contains("use --desc (protoc step in README)"),
            "args: {args:?}, stderr: {}",
            stderr(&out)
        );
    }
}

#[test]
fn describe_unknown_service_fails_with_stderr() {
    let out = run_pbcurl(&["describe", "--desc", GATEWAY_DESC, "shop.NoSuch"]);
    assert!(!out.status.success());
    assert!(!stderr(&out).trim().is_empty());
}
