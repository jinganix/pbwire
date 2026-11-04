# pbwire Go binding

Generated Go codecs for the pbwire wire format. `protoc-gen-pbwire-go` turns a
`.proto` file into plain Go structs plus straight-line `Size`, `Marshal` and
`Unmarshal` methods; there is no cgo, no descriptor set at run time, no
reflection and no runtime dependency beyond the small `runtime/` package of
unsigned-varint/fixed/tag primitives.

The generated code is the same shape `protoc-gen-go` produces -- one Go struct
per message, pointer fields where the field has explicit presence -- but the
codec writes and reads wire bytes directly instead of going through
`protoreflect`. That is what makes it roughly 1.6-3x faster than
`google.golang.org/protobuf` on the standard tiers (see
[`BENCHMARKS.md`](../../BENCHMARKS.md)).

| Piece | Path | Role |
|---|---|---|
| runtime | [`runtime/`](runtime/) | package `pbwire`: varint/fixed/tag primitives, unknown-field skipping, nesting cap |
| plugin | [`plugin/`](plugin/) | `protoc-gen-pbwire-go`, its golden tests and the regen/dump targets |
| tests | [`tests/`](tests/) | generated fixtures + `protoc`-pinned reference bytes (matrix2, matrix3, edge cases) |
| examples | [`examples/addressbook/`](examples/addressbook/) | generated `tutorial.AddressBook` round trip |
| benchmarks | [`benchmarks/`](benchmarks/) | four standard tiers vs `google.golang.org/protobuf` + vtproto generated code |
| conformance | [`conformance/`](conformance/) | why the generated-only model has no conformance testee |

```shell
make go-test                                   # runtime + plugin + generated-code tests
go run ./examples/addressbook                  # 59 bytes, name=alice id=1234 phones=2
make go-bench                                  # vs the official Go runtime (needs protoc + protoc-gen-go)
```

From your own project, install the plugin and point it at the runtime import
path:

```shell
cd bindings/go/plugin && go build -o $GOBIN/protoc-gen-pbwire-go ./cmd/protoc-gen-pbwire-go
protoc -I. --plugin=protoc-gen-pbwire-go=$GOBIN/protoc-gen-pbwire-go \
    --pbwire-go_opt=runtime_import=example.com/pbwire/runtime \
    --pbwire-go_out=. api.proto
```

## Generated API

```go
type Person struct {
	Name   *string            // proto2 required / optional, proto3 optional
	Id     *int32
	Email  *string
	Phones []Person_PhoneNumber // repeated: slice of values, not pointers
}

func (m *Person) Size() int
func (m *Person) Marshal() ([]byte, error)                 // allocates once
func (m *Person) MarshalAppend(b []byte) ([]byte, error)   // reuses the caller's buffer
func (m *Person) Unmarshal(b []byte) error                 // replaces m
```

- Presence is the field shape: a pointer for explicit presence (proto2
  optional/required, proto3 `optional`, message fields), a plain value for
  proto3 implicit presence, a slice for repeated fields. `bytes` uses `[]byte`
  with `nil` meaning absent, like `protoc-gen-go`.
- Repeated messages are `[]T` values, not `[]*T`: decoding a repeated message
  field is one allocation for the slice instead of one per element.
- `Size` and `Marshal` skip zero-valued implicit-presence fields (including
  `-0.0`, which the wire format keeps) and emit required fields unconditionally;
  a missing required field returns `pbwire.ErrRequired` from `Marshal` and
  `Unmarshal`.
- `Unmarshal` resets the message, merges duplicate occurrences of a singular
  message field (protobuf semantics) and caps nesting at `pbwire.MaxDepth`.
- Unknown fields -- and known fields carrying the wrong wire type -- are skipped
  and not preserved. That is the struct-binding contract (the C struct engine
  does the same); the dynamic core is where unknown-field preservation lives.

## Coverage

Every scalar kind, `string`, `bytes`, enums (closed and open), singular and
repeated messages, packed and unpacked repetition (both directions accepted on
decode), proto2 required/optional and proto3 `optional` presence, and editions
field presence. Every generated file is gofmt-clean.

Rejected with an explicit generator error, because the generated struct has no
representation for them: map fields, oneofs, groups and extensions. Message or
enum types imported from another `.proto` file are also rejected -- the plugin
has no import path for them, so keep one generated package per file. These are
the same boundaries the C struct binding documents (`bindings/c/plugin`); the
dynamic core still handles all of them.

Proto2 `[default = ...]` values are schema-level accessor semantics, not wire
data: an absent optional field stays `nil`, and `protoc-gen-go`-style `Get`
helpers are not generated. Unknown numeric values of a proto2 closed enum are
stored in the field as-is rather than moved to unknown fields; they re-encode
byte-identically.

## Tests

`tests/` holds generated code for `bindings/c/tests/data/{matrix2,matrix3}.proto`
and for the Go-only `tests/data/edges.proto` (bytes presence, `-0.0`, empty
packed runs, recursion), plus the `protoc --encode` reference bytes. The suite
checks byte equality with those vectors, decode and re-encode, wrong-wire-type
and unknown-field skipping, truncation safety, packed/unpacked acceptance,
required-field errors, singular-message merge and the nesting cap.

`make -C bindings/go/plugin test` replays the checked-in protoc requests
against the pinned generated files, so the generator itself is covered without
needing `protoc`.

## Benchmark

`benchmarks/` runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md) against `google.golang.org/protobuf`
generated code and vtproto (the fastest widely used Go generator) from the same
`matrix3.proto` (small / medium / large / xlarge
scale the repeated fields x0 / x1 / x100 / x3000). Copied from that file, which
is the single source for all performance numbers; higher is better, MB/s over
wire bytes:

| payload | op | pbwire-go (MB/s, x official) | official generated (MB/s) | vtproto (MB/s) |
|---|---|---|---|---|
| 136 B | encode | 1462.4 (2.20x) | 663.4 | 2092.3 |
| 136 B | decode | 1247.7 (2.13x) | 586.2 | 1387.8 |
| 174 B | encode | 1450.0 (2.34x) | 619.2 | 1933.3 |
| 174 B | decode | 887.8 (1.84x) | 482.0 | 1000.0 |
| 2469 B | encode | 1663.7 (3.06x) | 543.6 | 1800.9 |
| 2469 B | decode | 522.7 (1.67x) | 312.7 | 596.7 |
| 74913 B | encode | 1721.8 (2.90x) | 593.9 | 1967.0 |
| 74913 B | decode | 603.5 (1.73x) | 348.5 | 622.2 |

## Typed models

Go's generated codecs are already the typed model (`protoc-gen-pbwire-go` emits
the structs and their size/encode/decode), so the shared typed plugin in
`../typed/` has nothing to add here.
