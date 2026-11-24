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
- Request body: binary-encoded input message (`Content-Type: application/proto`).
- Response body: binary-encoded output message.
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
