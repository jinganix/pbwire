# pbnginx — fixed-path reverse proxy demo (CONTRACT)

> [!WARNING]
> RULING-RELEVANT FINDING: `examples/gateway.proto` defines **zero
> services and zero methods** (package `shop`; messages `Item`, `Order`
> only — verified with `protoc --decode` and `grep -c 'service '`). The
> plan's `shop.OrderService` names are placeholders. The service and
> method below are a **PROPOSED stub** so Tasks 2–5 have fixed paths to
> build against. If the fixture later gains a real `service` block, this
> table MUST be revised to match it exactly.

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
- Runtime schema entry is always `FileDescriptorSet` bytes
  (`examples/gateway.desc`), loaded at startup, never embedded.

## Path table

| Path | Input message | Output message | Status |
|---|---|---|---|
| /pb/shop.OrderService/PlaceOrder | shop.Order | shop.Order | PROPOSED stub — no `service` block in fixture |

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
