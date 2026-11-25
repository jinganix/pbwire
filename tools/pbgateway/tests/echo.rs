//! HTTP-level roundtrip tests for `pbgateway` against the real overlay
//! descriptor (`tools/pbnginx/gateway_svc.desc`: `shop.OrderService`,
//! `PlaceOrder(Order) returns (Order)`, echo semantics).
//!
//! Each test spawns the real server binary on 127.0.0.1 with an ephemeral
//! port and speaks raw HTTP/1.1 over `TcpStream` (one connection per request,
//! server always answers `Connection: close`).

use std::io::{Read, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::time::Duration;

/// Overlay fixture: package `shop`, `service OrderService {
/// rpc PlaceOrder(Order) returns (Order); }`.
const DESC: &str = "../pbnginx/gateway_svc.desc";

/// `Order{id: "o-1"}` plus an unknown field 99 (varint 7):
/// `0A 03 "o-1"` then tag `(99 << 3) | 0` = `98 06`, value `07`.
const PINNED_REQ_WITH_F99: &[u8] = &[
    0x0A, 0x03, b'o', b'-', b'1', // field 1 (id): "o-1"
    0x98, 0x06, 0x07, // field 99 (unknown to shop.Order): varint 7
];

const ECHO_PATH: &str = "/pb/shop.OrderService/PlaceOrder";
const MAX_BYTES: usize = 4 * 1024 * 1024;

struct Resp {
    status: u16,
    body: Vec<u8>,
}

fn free_port() -> u16 {
    let l = std::net::TcpListener::bind("127.0.0.1:0").expect("bind ephemeral");
    l.local_addr().expect("local addr").port()
}

fn start_server(port: u16) -> Child {
    let bin = env!("CARGO_BIN_EXE_pbgateway");
    let mut child = Command::new(bin)
        .args(["--desc", DESC, "--port", &port.to_string()])
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn pbgateway");
    // Wait until the listener accepts (or the process dies).
    for _ in 0..200 {
        if let Ok(s) = TcpStream::connect(format!("127.0.0.1:{port}")) {
            drop(s);
            // The probe connection consumes one accept; give the server a
            // moment to return to accept before the real request arrives.
            std::thread::sleep(Duration::from_millis(10));
            return child;
        }
        if child.try_wait().expect("wait").is_some() {
            panic!("pbgateway exited before listening");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let _ = child.kill();
    panic!("pbgateway did not listen on 127.0.0.1:{port} in time");
}

fn post_raw(path: &str, body: &[u8], accept_json: bool) -> Resp {
    let port = TEST_PORT.with(|p| *p.borrow());
    let mut s = TcpStream::connect(format!("127.0.0.1:{port}")).expect("connect");
    s.set_read_timeout(Some(Duration::from_secs(5)))
        .expect("timeout");
    let accept = if accept_json {
        "Accept: application/json\r\n"
    } else {
        ""
    };
    let head = format!(
        "POST {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n{accept}Content-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    s.write_all(head.as_bytes()).expect("write head");
    s.write_all(body).expect("write body");
    let mut raw = Vec::new();
    s.read_to_end(&mut raw).expect("read resp");
    parse_resp(&raw)
}

fn parse_resp(raw: &[u8]) -> Resp {
    let sep = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .expect("response head");
    let head = std::str::from_utf8(&raw[..sep]).expect("head utf8");
    let status: u16 = head
        .lines()
        .next()
        .expect("status line")
        .split_whitespace()
        .nth(1)
        .expect("status code")
        .parse()
        .expect("status number");
    Resp {
        status,
        body: raw[sep + 4..].to_vec(),
    }
}

use std::cell::RefCell;
thread_local! {
    static TEST_PORT: RefCell<u16> = const { RefCell::new(0) };
}

/// Kills the server even when the test body panics (no leaked listeners
/// holding ports across runs).
struct ServerGuard {
    child: Child,
}

impl Drop for ServerGuard {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn with_server(f: impl FnOnce()) {
    let port = free_port();
    TEST_PORT.with(|p| *p.borrow_mut() = port);
    let child = start_server(port);
    let _guard = ServerGuard { child };
    f();
}

#[test]
fn echo_preserves_unknown_field_99() {
    with_server(|| {
        let resp = post_raw(ECHO_PATH, PINNED_REQ_WITH_F99, false);
        assert_eq!(resp.status, 200);
        // Field 99 tag (0x98 0x06) survives the decode -> re-encode roundtrip.
        assert!(
            resp.body.windows(2).any(|w| w == [0x98, 0x06]),
            "field 99 tag missing from echo body: {:02x?}",
            resp.body
        );
    });
}

#[test]
fn echo_binary_roundtrip_preserves_id() {
    with_server(|| {
        let resp = post_raw(ECHO_PATH, PINNED_REQ_WITH_F99, false);
        assert_eq!(resp.status, 200);
        // "o-1" survives alongside the unknown field.
        assert!(
            resp.body.windows(3).any(|w| w == b"o-1"),
            "id missing from echo body: {:02x?}",
            resp.body
        );
    });
}

#[test]
fn echo_json_on_accept() {
    with_server(|| {
        let resp = post_raw(ECHO_PATH, PINNED_REQ_WITH_F99, true);
        assert_eq!(resp.status, 200);
        let text = std::str::from_utf8(&resp.body).expect("json utf8");
        assert!(
            text.contains("o-1"),
            "json echo missing id: {text}"
        );
    });
}

#[test]
fn unknown_path_is_404_text() {
    with_server(|| {
        let resp = post_raw("/pb/shop.OrderService/Nope", PINNED_REQ_WITH_F99, false);
        assert_eq!(resp.status, 404);
        assert!(!resp.body.is_empty(), "404 should carry a text body");
    });
}

#[test]
fn bad_bytes_are_400_text() {
    with_server(|| {
        // 0xFF... is not a valid Order encoding (truncated varint/tag).
        let resp = post_raw(ECHO_PATH, &[0xFF, 0xFF, 0xFF], false);
        assert_eq!(resp.status, 400);
        assert!(!resp.body.is_empty(), "400 should carry a text body");
    });
}

#[test]
fn oversize_body_is_rejected() {
    with_server(|| {
        // Advertise a body past the 4 MiB cap but send only a prefix, then
        // half-close: the server must reject on Content-Length alone without
        // waiting for (or reading) the rest. Writing the full 4 MiB into a
        // server that already answered races with its RST and is pointless.
        use std::net::Shutdown;
        let port = TEST_PORT.with(|p| *p.borrow());
        let mut s = TcpStream::connect(format!("127.0.0.1:{port}")).expect("connect");
        s.set_read_timeout(Some(Duration::from_secs(5)))
            .expect("timeout");
        let head = format!(
            "POST {ECHO_PATH} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            MAX_BYTES + 1
        );
        s.write_all(head.as_bytes()).expect("write head");
        s.write_all(&[0u8; 1024]).expect("write prefix");
        s.shutdown(Shutdown::Write).expect("shutdown");
        let mut raw = Vec::new();
        s.read_to_end(&mut raw).expect("read resp");
        let resp = parse_resp(&raw);
        assert!(
            resp.status == 413 || resp.status == 400,
            "oversize body must be rejected, got {}",
            resp.status
        );
    });
}
