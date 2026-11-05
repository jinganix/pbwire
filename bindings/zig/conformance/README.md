# Zig conformance testee

`testee.zig` is the official-suite testee driven by the Zig binding: it reads
`ConformanceRequest` messages from stdin and writes `ConformanceResponse`
messages to stdout, both length-delimited by a 4-byte little-endian length,
exactly like `conformance/testee.c`.

The two envelope messages are hand-built descriptors (mirroring `testee.c`'s
field numbers); the message under test always comes from the descriptor set on
the command line:

```shell
zig build testee   # zig-out/bin/testee-zig
./zig-out/bin/testee-zig <descriptor-set>   # from the repo root
```

Covered: binary in -> binary out, JSON and text in both directions (the core's
own printers/parsers, so unknown-field and canonical-form behavior matches the
C testee). JSPB answers `skipped`, like the C testee. A request that fails to
decode answers `runtime_error`; the two binary tag-truncation cases the
reference C++ testee fails identically are covered by the shared
`conformance/failure_list_pbwire.txt` (same core decoder, same two failures).
