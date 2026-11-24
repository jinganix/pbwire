#!/usr/bin/env bash
# e2e through nginx: pbgateway on :8081 behind nginx on :8080.
# SKIP-clean (exit 0) when the nginx binary is absent in the build env.
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DESC="$ROOT/tools/pbnginx/gateway_svc.desc"
CONF="$ROOT/tools/pbnginx/nginx.conf"
PBCURL_BIN="$ROOT/tools/pbcurl/target/debug/pbcurl"
GATEWAY_BIN="$ROOT/tools/pbgateway/target/debug/pbgateway"
GW_PORT=8081
NGINX_PORT=8080

cleanup() {
  if [ -n "${GW_PID:-}" ] && kill -0 "$GW_PID" 2>/dev/null; then
    kill "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
  fi
  if [ "${NGINX_STARTED:-0}" = "1" ] && [ -f "$CONF" ]; then
    nginx -c "$CONF" -s stop 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "== build pbcurl + pbgateway =="
cargo build --manifest-path "$ROOT/tools/pbcurl/Cargo.toml" --quiet || exit 1
cargo build --manifest-path "$ROOT/tools/pbgateway/Cargo.toml" --quiet || exit 1

echo "== start pbgateway on :$GW_PORT =="
rm -f /tmp/pbnginx.pid
"$GATEWAY_BIN" --desc "$DESC" --port "$GW_PORT" &
GW_PID=$!
for _ in $(seq 1 100); do
  if (echo > /dev/tcp/127.0.0.1/$GW_PORT) 2>/dev/null; then break; fi
  sleep 0.05
done
if ! (echo > /dev/tcp/127.0.0.1/$GW_PORT) 2>/dev/null; then
  echo "FAIL: pbgateway did not come up on :$GW_PORT" >&2
  exit 1
fi

if ! command -v nginx >/dev/null 2>&1; then
  echo "SKIP: nginx binary not found in PATH; cannot verify reverse-proxy path (gateway-only run not attempted here)." >&2
  exit 0
fi

if [ ! -f "$CONF" ]; then
  echo "FAIL: $CONF missing; cannot start nginx reverse proxy" >&2
  exit 1
fi

echo "== start nginx on :$NGINX_PORT =="
nginx -t -c "$CONF" || exit 1
nginx -c "$CONF" || exit 1
NGINX_STARTED=1
for _ in $(seq 1 100); do
  if (echo > /dev/tcp/127.0.0.1/$NGINX_PORT) 2>/dev/null; then break; fi
  sleep 0.05
done
if ! (echo > /dev/tcp/127.0.0.1/$NGINX_PORT) 2>/dev/null; then
  echo "FAIL: nginx did not come up on :$NGINX_PORT" >&2
  exit 1
fi

echo "== invoke via nginx :$NGINX_PORT =="
OUT="$(PBURL_BASE="http://127.0.0.1:$NGINX_PORT" "$PBCURL_BIN" invoke --desc "$DESC" shop.OrderService/PlaceOrder '{"id":"ord-1"}')" || {
  echo "FAIL: pbcurl invoke via nginx failed" >&2
  exit 1
}
echo "$OUT"
echo "$OUT" | grep -q 'ord-1' || { echo "FAIL: echoed field missing in: $OUT" >&2; exit 1; }
echo "PASS: echo round-trip through nginx :$NGINX_PORT"
