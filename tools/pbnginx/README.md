# pbnginx — fixed-path reverse proxy demo (CONTRACT)

Fixture note: `examples/gateway.proto` (package `shop`) defines
messages `Item` and `Order` only — no `service` block (per controller
ruling it stays untouched). The service below is therefore declared in
the overlay fixture `tools/pbnginx/gateway_svc.proto`, which imports
`examples/gateway.proto` and defines `service OrderService` with echo
semantics (`PlaceOrder`: `shop.Order` → `shop.Order`), matching the
plan's v1 echo gateway.

## Fixed-path scheme

Every RPC is plain HTTP `POST` to a fixed path — no query params, no
path variables, no transcoding:

```text
POST /pb/<service>/<method>
```

- `<service>` is the fully qualified proto service name
  (`<package>.<Service>`, e.g. `shop.OrderService`).
- `<method>` is the proto `rpc` name exactly as declared (case-sensitive).
- Request body: raw binary protobuf body (Content-Type unset in v1).
- Response body: binary-encoded output message.
- `Accept: application/json` returns JSON mapping.
- Errors are HTTP status + plain-text body.
- Runtime schema entry is always `FileDescriptorSet` bytes.
  Later tasks load `tools/pbnginx/gateway_svc.desc` (generated with
  `--include_imports`, so it embeds `shop.Order`/`shop.Item` via the
  import), never `examples/gateway.desc` alone, never embedded code.

## Path table

| Path | Input message | Output message |
|---|---|---|
| /pb/shop.OrderService/PlaceOrder | shop.Order | shop.Order |

## Pinned request vectors (`protoc --encode`, proto3 canonical)

Generated from `examples/` with
`protoc --encode=<type> gateway.proto` (fixture `gateway.desc` verified
byte-identical via `cd examples && protoc --descriptor_set_out=... gateway.proto`).

Canonical overlay command (run from repo root so the import resolves):

```sh
protoc --include_imports --descriptor_set_out=tools/pbnginx/gateway_svc.desc tools/pbnginx/gateway_svc.proto
```

`shop.Order` (`id: "ord-1"`, one `Item(sku-9, 3)`, `attrs{color:red}`,
`status: PLACED` — exercises nested message + map + enum):

```text
0a056f72642d3112090a05736b752d3910031a0c0a05636f6c6f7212037265642001
```

`shop.Item` (`sku: "sku-9"`, `qty: 3`):

```text
0a05736b752d391003
```

Tasks 3–5 MUST use the `shop.Order` vector above as the reference
request body for `POST /pb/shop.OrderService/PlaceOrder`.

## Follow-along demo (pbgateway behind nginx)

Prerequisite: an `nginx` binary on `PATH` (`nginx -v` must work).
Without it the e2e script `tools/pbcurl/tests/e2e_gateway.sh`
SKIPs cleanly (exit 0 with a `SKIP: nginx binary not found` message)
rather than failing, so CI without nginx stays green.

All commands run from the repo root.

1. Generate the descriptor set (overlay embeds `shop.Order`/`shop.Item`):

```sh
protoc --include_imports --descriptor_set_out=tools/pbnginx/gateway_svc.desc tools/pbnginx/gateway_svc.proto
```

2. Start the echo gateway on `:8081`:

```sh
cargo build --manifest-path tools/pbgateway/Cargo.toml
./tools/pbgateway/target/debug/pbgateway --desc tools/pbnginx/gateway_svc.desc --port 8081 &
```

3. Start nginx on `:8080` (`proxy_pass 127.0.0.1:8081` for `/pb/`):

```sh
nginx -t -c "$PWD/tools/pbnginx/nginx.conf"
nginx -c "$PWD/tools/pbnginx/nginx.conf"
```

4. Invoke through nginx (echoed field `ord-1` proves the round-trip):

```sh
cargo build --manifest-path tools/pbcurl/Cargo.toml
PBURL_BASE=http://127.0.0.1:8080 ./tools/pbcurl/target/debug/pbcurl invoke \
  --desc tools/pbnginx/gateway_svc.desc shop.OrderService/PlaceOrder '{"id":"ord-1"}'
```

Or run the whole path as one script:

```sh
bash tools/pbcurl/tests/e2e_gateway.sh
```

5. Stop everything (unload = stop gateway, then stop nginx):

```sh
nginx -c "$PWD/tools/pbnginx/nginx.conf" -s stop
kill %1  # the pbgateway background job (or `kill <pid>`)
```
