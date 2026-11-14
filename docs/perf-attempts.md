# Performance attempts ledger

This page records every optimization tried against a cell that is **below its
baseline** in [BENCHMARKS.md](../BENCHMARKS.md), whether it was kept or
rejected, and why. It exists so the same ground is not re-ploughed: several of
these attempts look obviously right on paper and measure worse on a real
machine.

Read [BENCHMARKS.md](../BENCHMARKS.md) first for the standard cases, the
harness names and the current numbers. This page only adds the experiments.

## How an attempt is measured

Same method as the core's notes in BENCHMARKS.md:

1. Build the unchanged tree and the candidate as two binaries/class dirs from
   the same source tree (`base` and `vN`).
2. Alternate the two in one session (same machine, same JIT/heap state), so a
   drifting machine hits both sides equally. Anything below ~5% is noise
   unless the two sides differ in every round.
3. Keep only changes that win in every round. A change that wins in one round
   and loses in the next is recorded as neutral and reverted.
4. For the pure-Java codec the probe is a classpath A/B
   (`benchmarks/ab.sh <base-classes> <cand-classes>`): `Ab` loads the same
   `matrix3.desc` and the protobuf-java oracle, then reports best-of-passes
   ns/op per label for dyn-encode/dyn-decode against pbuf-encode/pbuf-decode
   on small/const/varied shapes. `-XX:+UnlockDiagnosticVMOptions
   -XX:+PrintInlining` confirms whether the candidate still inlines.

## Cell: Java pure-Java codec, 136 B encode (0.73x)

BENCHMARKS.md lists `protobuf-java` 58.3 ns / 136 B against `pbwire-java`
75.1 ns; the Kotlin binding drives the same codec (`Message.encode()` ->
`GenericCodec.encode`), which is why it sits on the same floor (0.80x).

Baseline in the A/B probe: 74.3-82.0 ns per encode (the session drifted later;
only in-round comparisons below are meaningful).

### Where the time goes

Measured by ablation: each variant removes one kind of work from
`encodeInto()` and the rest stays. Bytes differ in those runs, so they are
measurement only - the real benchmark gates on byte equality first.

| removed from the encode | ns/op | cost of that part |
|---|---|---|
| nothing (base) | 74.3 | - |
| all singular numeric writes | 45.6 | 28.7 |
| both string fields | 63.4 | 10.9 |
| the nested `inner` message | 68.1 | 6.2 |
| all field tag bytes | 69.1 | 5.2 |
| the `Arrays.copyOf` result copy | 76.2 | ~0 (inside the noise floor) |

The remainder, 23.3 ns, is the per-field skeleton alone: 22 iterations of
`has[]` load, `Field` load, label/kind/presence tests and dispatch, with no
byte written at all - 1.06 ns per field. `protobuf-java` has no such loop: its
generated `writeTo` is straight-line code with compile-time offsets.

### Attempts (all rejected)

| # | candidate | result | why |
|---|---|---|---|
| 1 | unrolled branch-tree varint writer, `putVar32U`/`putVar64U` (the C core's shape: test, store, shift, repeat) | 105-128 ns (+35..+55%) | `putVar32U` grew to 200 bytecode bytes and still inlined, `putVar64U` to 437 and **stopped inlining** ("hot method too big"). The call per varint costs more than the loop it removed. |
| 2 | same loop, but plain `a[i++] = ...` stores instead of `Unsafe.putByte` | 91-140 ns (+15..+70%) | the `Unsafe` byte store is at least as cheap as a bounds-checked array store; the int cursor also adds a cast per store. |
| 3 | packed varint: build the 8-byte image with shifts/ors, one `Unsafe.putLong`, then advance by the computed byte count (2-5 bytes through `putInt`) | 120-161 ns (+60..+95%) | stays inlined (144/250 bytes) and is still far worse in the real encoder: the packing chain adds register pressure to an already large inlined `encodeInto`, so the isolated microbenchmark result (0.33 vs 0.41 ns/byte) does not transfer. |
| 4 | primitive mirror arrays on `MessageDef` (`kind[]`, `presence[]`, `repeated[]`, `tags[]`) instead of pointer chasing through `Field` | 84-155 ns (+5..+70%) | four arrays with four bases cost more loads than two loads plus the `Field` fields the JIT already had in L1. |
| 5 | inline the singular ref path (string/bytes/message) from `writeRefVal()` into `encodeInto()` | 85-98 ns (+9..+15%) | removes three call boundaries but enlarges `encodeInto`, which is already the only reason `writeRefVal` sometimes fails to inline. |
| 6 | drop the `f.repeated` branch and the `v == 0 && IMPLICIT` test (measurement only: the x0 payload has no repeated fields and no zero values) | 76.0 vs 75.6 | neutral. These per-field branches are not the cost, which is why a precomputed action table would not have paid off either - it was dropped before being built. |

### Regression caught by the tables (revert kept)

The 8-slot `StrCache` (whole-field images keyed by tag/string identity)
became a 1-slot `StrMemo` (remembers only the last pair, writes anything
else in place). Rationale: varied traffic should not allocate images it
never reuses. Measured on the 136 B tier, where the same three strings
repeat every encode: 81-89 ns -> 110-115 ns per encode (alternating A/B,
`bindings/java/benchmarks/ab.sh`), and the table cell fell 0.82x -> 0.33x
before the machine noise is accounted for. Restoring the cache recovered
it (0.73x, within noise of the old floor; the xlarge tier never moved
either way, since 3000 identical elements hit both designs). Lesson: the
benchmark payloads are stable-value traffic, and that is also what the
`encode(parsed)` columns measure — a cache that only helps varied traffic
helps no published cell.

Two reusable facts came out of this:

- **HotSpot's inlining ceiling is the binding constraint.** A helper inlined
  into `encodeInto()` must stay under ~325 bytecode bytes
  (`FreqInlineSize`); at 437 the call site reports "hot method too big" and the
  win evaporates. Any unrolled writer has to be measured with
  `-XX:+PrintInlining`, not reasoned about.
- **The per-field dispatch is the floor.** Removing all of a field's byte
  writes still leaves 1.06 ns/field of loop and dispatch, and `protobuf-java`
  spends none of that. That is the "closing it needs per-message codegen" note
  in `bindings/java/runtime/GenericCodec.java`, now with numbers behind it.

## Cell: Julia, 136 B decode (0.92-0.95x)

Official harness (`make julia-bench`, same session): `pbwire-julia` 133.2 ns,
`protobuf-jl` 126.1 ns - a 7.1 ns gap, i.e. 5% of the cell.

Split with a C probe on the same 136-byte payload (best of 5 x 200k
`pbw_decode` + `pbw_message_free`): **121 ns**, so the C core is the whole
cell and the Julia wrapper around it (one `ccall`, one `Ref`, one `Message`
struct, `close`) is ~12 ns.

`sample` on the C probe (8 s, leaf frames, share of all samples):

| frame group | share |
|---|---|
| `decode_fields` (tag read, fnum lookup, action switch) | 40% |
| `pbw_varint_read` (out-of-line call for values over 2 bytes) | 12% |
| `malloc`/`free`, mostly macOS's xzone free path | 16% |
| `read_field`, `store_first_or_put`, `store_bytes_first`, `pbw_message_put` | 12% |
| `memcpy`/`memset` (string copies, tree init) | 8% |
| `pbw_message_new_a`, arena, `pbw_decode` entry | 6% |

`malloc` + `free` of one ~600-byte block measures **15.7 ns** standalone on
this machine (macOS 27, M4 Pro); the free side dominates and calls
`mach_absolute_time` inside `__xzm_xzone_free_to_freelist_chunk`. That is the
"one malloc/free pair per plain call" that
[docs/design.md](design.md#two-allocation-strategies) documents, and for a
136-byte message it is ~12% of the decode.

Rejected before building:

- **`pbw_ctx` pooling on the Julia side** (one context per decoded message,
  returned to a pool on `close`) would remove the malloc/free pair, but
  `pbw_decode_ctx` makes the tree read-only, and the binding's `decode`
  promises a mutable message. Rejected on semantics, not on speed.
- **Inlining more of `pbw_varint_read`** into `read_varint` (the 3-5 byte
  window) covers only two of this payload's nine varints (~2-3% of decode),
  while adding code to every inlined copy of `read_varint` in `decode_fields`.
  Not worth the icache risk for a cell that needs 5%; the 10-byte path is
  already unrolled in `pbwire_varint_read` and its cost is the out-of-line
  call, not the shifts.

## Cell: Kotlin, 136 B encode (0.82x) and 174 B encode (0.53x)

The same codec as the Java cell above (`bindings/kotlin/runtime` adds DSL
sugar over `bindings/java/runtime`; `Message.encode()` ends in
`GenericCodec.encode`), so attempts 1-6 apply unchanged and were not repeated.
The 174 B cell does not follow the Java row (1.17x) on identical bytes: a
same-JVM A/B of the two message-build styles shows no codec-side cause, and
a fixed-heap SerialGC run recovers most of it, so it is the harness's
allocation/GC sensitivity on a loaded host, not the codec.

## Regression: Python `decode()`, 74913 B (1.70x -> 1.41x)

The 0.1.15 required-field work put the required-presence bookkeeping into
`scan_message()`'s per-field loop (`bindings/python/runtime/pbwire.c`), and
`decode()` runs that loop once per wire field. A/B on one host, alternating
builds, 74913 B, and the upb row in the same process (the ratio is the column
to read; absolute MB/s moves 8-10% between runs and upb moves with it):

| runtime | pbwire-py / upb, xlarge decode |
|---|---|
| 0.1.14 lazy scan | 1.76x-1.78x |
| 0.1.15 (HEAD) | 1.39x-1.41x |
| 0.1.15, presence update compiled out (measurement only) | 61.2-64.3 k ns -> 55.0-55.8 k ns per decode, ~10-13% |

The checks never run (proto3 types have no required field); the ~10-13% is
the code shape of the loop, and it lands on the recording walk (`m != NULL`) even
though only the validating walk (`m == NULL`, nested messages) needs the
tracking. Tried and rejected as neutral: moving the required check out of line
into `scan_required()` (1.39x-1.41x, unchanged), and hoisting the "validating
only" early `continue` back above the field lookup (the one measured win,
1.44x, is unsafe there: a required scalar in a nested message is only seen
after the update, so the check would reject a valid wire).

Closing it means splitting the validating walk from the recording walk, so the
recording loop drops the required update and marks presence from `m->slices`
afterwards. That is a second wire walk in the file; do it with
`make python-test` and `make conformance-python`, and add a bench gate on this
cell first, because the tables were the only thing that caught it. The python
tests only cover a required field on the top-level message, not behind a
nested one.

### Attribution corrected: it is the loop frame, not the update

Built and measured the move described above (required update only under
`m == NULL`, presence marked from `m->slices` after the recording walk, no
second wire walk). It changes nothing: 1.32x vs HEAD's 1.35x against upb on
the same host, three alternating rounds of two `.so` builds in one process.
The per-field update is not where the cost is, in either walk:

| runtime | 74913 B decode |
|---|---|
| HEAD (0.1.15) | 67.7 / 69.9 / 70.0 k ns |
| update moved to the validating walk (above) | 72.2 / 70.0 / 71.9 k ns |
| update kept only on the recording walk (measurement only) | 70.2 k ns |
| `check_required` compiled to 0 (measurement only) | 58.3 / 61.4 / 60.9 k ns |

The last row is the only one that wins (~12%), and it deletes the mask, the
over-64 byte array, the tail validation and the branch from the function body
at once. Removing the branch from either walk alone buys nothing, so the cost
is the liveness of `required_seen` / `required_fields` across the shared loop
frame, not the work per field. One loop cannot carry required state for the
validating walk without paying it on the recording walk too, so the win needs
two loop bodies: duplicated tag/length/group handling in a validating parser.
That is why it stays a documented floor instead of a quick patch.

## Whole-vector attempt: build flags (`-O3`) for every binding

Aim: +10% on both ops for every binding from one generic change. The only
change with that reach is a build flag, because 20 of the 23 targets compile
the same `src/*.c` (Java, Kotlin, Scala and Clojure run their own pure-Java
codec, one runtime shared by all four). `-O2` is what the Makefile and CMake
pin, so `-O3` was measured against it: the same sources compiled twice into
`libpbwire.a`, one bench binary per build, alternating rounds in one session,
fixed harness counts (400000 / 200000 / 8000) so each tier is comparable.
Median ns/op over three rounds, `pbw_ctx` rows:

| tier | op | `-O2` | `-O3` | delta |
|---|---|---|---|---|
| 132 B | encode | 66.0 | 68.7 | +4% (slower) |
| 132 B | decode | 87.7 | 86.5 | noise |
| 169 B | encode | 97.3 | 93.4 | -4% |
| 169 B | decode | 138.6 | 137.7 | noise |
| 2755 B | encode | 1914.4 | 1877.2 | -2% |
| 2755 B | decode | 3579.0 | 3464.1 | -3% |
| 85933 B | encode | 61660.3 | 54523.8 | **-11.6%** |
| 85933 B | decode | 104940.5 | 105257.8 | noise |

Read it as: `-O3` buys one cell (xlarge encode, where the writer's loops are
big enough for the extra unrolling and vectorisation to pay), leaves decode
flat everywhere, and is a wash or slightly worse on the small tiers, which are
the common real case. Against the goal (+10% on both ops for every binding)
that is not close: decode never moves, and the four pure-JVM targets would
need JVM-side work that no C flag reaches. Not taken: a build flag changes
every binding's build step and the CI matrix (`-m32`, big-endian, MSVC), and
`-O3` re-enables optimisations that only a full sanitizer and port run would
re-certify -- for one tier of one op. The same measurement also rules out the
"make everything 10% faster with a flag" shortcut in general: the codec's
remaining cost is the per-field loop shape, which is what
the entries above already tried and failed to buy.

## What would actually close these cells

- Java/Kotlin 136 B encode: per-message-type codegen (a generated
  straight-line `encodeInto` per message, i.e. what `protoc` emits). The
  binding rejects that by design; it is a product decision, not a tuning
  session.
- Julia 136 B decode: something that removes the per-call `malloc`/`free`
  pair from the plain path, which is an ownership change (see
  [docs/design.md](design.md#two-allocation-strategies)), plus ~12 ns of Julia
  wrapper that a `ccall`-only design cannot avoid.

## Re-run round: the sub-1.0x cells, 2026-11-14

Every binding with a below-baseline cell was re-run (core, Go, Java, Kotlin,
Clojure, Ruby, Node, Julia) on a host that carried 9-31 load average from
unrelated processes, so the runs are no quieter than the ones already in the
tables. The cells did not flip; the spread across the day's runs is:

| cell | ratio across runs | note |
|---|---|---|
| Java 136 B encode | 0.71x-0.93x | pure-Java writer vs generated straight-line code |
| Kotlin 136 B encode | 0.78x-0.97x | same runtime, plus one DSL call |
| Kotlin 174 B encode | 0.79x | GC-sensitive, see its cell above |
| Clojure 136 B encode | 0.70x-0.75x | interop path, same Java codec |
| Julia 136 B decode | 0.89x-0.94x | plain-path `malloc`/`free` plus `ccall` (0.93x after the UTF-8 pass) |
| core plain 169 B encode | 0.94x-1.04x | one `malloc`/`free` pair per call (0.94x after the UTF-8 pass) |
| Go 136 B encode | 0.94x-1.00x | one cgo downcall per op |
| Go 174 B encode | 0.98x | same |
| Ruby 174 B encode | 0.95x-1.19x | bandwidth-bound at 174 B (1.00x after the UTF-8 pass) |
| Node 136 B decode | 0.96x-1.14x | one N-API downcall per op (1.00x after the UTF-8 pass) |

One more generic candidate was measured and rejected this round: the plain
encode path starts its output buffer at 64 bytes and doubles, so a 132-139 B
message pays `malloc(64)` + two `realloc`s per call. Raising the initial
capacity to 256 (which would remove both reallocs for every binding that calls
`pbw_encode`) measures nothing: a C probe on a 139 B message gives 34.5-37.9 ns
for both builds across five alternating rounds (median 35.8 vs 35.2), inside
the noise floor. The reallocs are a few ns against a ~30 ns tree walk.

One cell *was* actionable that round and is now fixed: the UTF-8 validator
below was a byte-per-iteration scan, which cost the Objective-C 74 KB tier
0.14x and every string-heavy decode row several percent. Rows re-measured
after that pass (BENCHMARKS.md, 2026-11-14):

| cell | before | after |
|---|---|---|
| Objective-C xlarge decode | 0.14x | 1.16x |
| Objective-C large decode | 0.79x | 2.62x |
| Python xlarge decode | 0.93x-1.38x | 1.22x |
| C struct xlarge decode | 1.72x | 2.00x |
| Node 136 B decode | 0.96x | 1.00x |
| Julia 136 B decode | 0.89x-0.94x | 0.93x |
| Elixir xlarge decode | 1.38x | 0.99x (baseline matched the core) |

What is left is not tuning: the four JVM cells need per-message codegen (a
product decision this project rejects, see the Java cell above), Julia's decode
cell needs a context-based API (an ownership change), and the rest are boundary
cells whose ✅/❌ flips with host load at the 136-174 B tier. The 2026-11-14
refresh added two more of those at the largest tier, both inside their
baseline's own run-to-run band: Java xlarge encode 0.95x and Rust xlarge encode
0.98x (prost's one-pass writer at 1333 MB/s against the core's 1305), and
Elixir's xlarge decode 0.99x. The tables keep the strict `>= 1.0x` rule the
project asks for: a cell under 1.0x is marked ❌ even when it is inside the
noise band, and the text says so.

## Aligned cost: proto3 `string` UTF-8 validation

pbwire used to keep any bytes in a `string` field. The reference does not:
`FieldDescriptor::requires_utf8_validation()` is true for a `string` whose
`utf8_validation` resolves to VERIFY (proto3 and edition-2023+ by default, proto2
never), and libprotobuf's parser rejects such a message ("contains invalid UTF-8
data"), as does protobuf-java's `readStringRequireUtf8`. Aligning meant
validating at parse, in every decode path (core `read_string_field` plus the
`PBW_ACT_STR` / `PBW_ACT_REP_STR` fast paths, the C struct codec, the Java scan
and the Python scan).

Green path (byte equality, conformance, differential test) is unchanged; the
cost is decode time on payloads with many proto3 strings. A/B on the C struct
harness over `matrix3` (proto3, 3000 sub-messages each with a string), two libs
from the same sources, validation on versus forced off, alternating runs, best
of 5, median ns per decode:

| tier | validation on | validation off | delta |
|---|---|---|---|
| 136 B | 108.3 | 108.8 | noise |
| 174 B | 165.9 | 154.9 | +7.1% |
| 74913 B | 99800.7 | 87440.9 | **+14.1%** |

That is the aligned price: the reference pays it too (libprotobuf's own decode
of the same payload is 191278 ns in that harness, so pbwire still leads by ~1.9x
instead of ~2.2x). Behaviour proved against the reference rather than argued:
the same three bytes (tag 1, length 1, 0xFF) parse as an error in a proto3
message and as data in a proto2 one, on both sides, and the binary conformance
suite now passes 2750 tests instead of 2745 (the extra ones are the JSON/UTF-8
verdicts this alignment fixed). The cost measured here was the implementation's
(one byte per iteration); the follow-up below keeps the rule and removes
almost all of the time.

### Follow-up: the validator itself (kept)

The 14.1% price above was the *implementation*, not the rule: `pbw_utf8_valid`
walked one byte per iteration, so every ASCII value paid a per-byte branch and
a 74 KB ASCII value (the Objective-C tier shape) cost ~20 us of the decode. Two
generic changes, measured against the pre-change library with two binaries
built from the same tree and alternated in one session (best of 9 rounds):

1. **ASCII runs skip 16 bytes at a time.** A word whose bytes are all below
   0x80 has no high bit set, on either endianness, so two `uint64` loads and
   one `or`/`and` test cover 16 bytes; the 8-byte loop and a byte loop finish
   the remainder. The multi-byte rules now live in one `pbw_utf8_seq` helper
   (internal.h) instead of being spelled out in each branch.
2. **Values shorter than a machine word are validated inline** at the
   decoder's three call sites (`pbw_utf8_ok` in internal.h). A 3-byte name or
   8-byte tag was paying an out-of-line call for three iterations.

C probe (alternating A/B, best-of-9, ns per pass):

| case | old scalar | new word scan | new inline short path |
|---|---|---|---|
| 6000 short strings (3/8 B) | 15390 | 13027 | 10999 |
| one 75776 B ASCII value | 19974 | 1274 | 1257 |

End to end, `pbw_decode` + `pbw_message_free` on a real `matrix3` payload
(protoc-encoded, 74889 B, 6000 strings), best of 5 x 300 iterations:

| build | plain decode | `pbw_ctx` decode |
|---|---|---|
| old | 106.4 us | 121.3 us |
| now | 99.6 us | 117.0 us |

and on the Objective-C tier shape (one 74004-byte ASCII string), one
decode+free per iteration: 20849 -> 2141 ns per decode (9.7x). The
diagnosis that led here was the 2026-11-14 table row: `pbwire-objc` xlarge
decode had fallen to 0.14x of the ObjC runtime purely on that 74 KB scan, and
the harness now reports 1.16x with everything else unchanged.

Cost/benefit for the rest of the matrix: the `matrix3` payload is 6000 short
strings, where the word scan is worth ~4% of the decode and the inline path
another ~3%. That is enough to move the C struct xlarge decode row from 1.72x
to 2.00x; on the Python row the same ~7% is smaller than the day-to-day drift
of the upb baseline (the 1.22x quoted in the table comes from one run, and upb
moved ~10% between sessions), so it does not read as a validator effect there.
The core's own decode rows landed at 1.52x-1.82x. Correctness is
carried by `tests/test_pbwire.c:test_utf8_validator`, which walks every ASCII
length across the 8- and 16-byte boundaries, every bad sequence at every offset
of a 24-byte value, truncated tails and a 300-byte value with a bad byte at
either end, with hand-written expectations (independent of the scan), plus the
existing decode verdict tests, `make difftest` and `make conformance`
(2750 binary/JSON + 443 text, unchanged).

## Cell: Go, 136 B / 174 B encode (0.94x) — kept

The two small encode cells were the only cells left where the *binding shape*
was the whole gap, not the codec. `Encode()` called `pbw_encode()` once (which
mallocs the result and hands ownership over), then `pbw_free()` (a second
downcall), then `C.GoBytes()` (a Go allocation plus a full copy) — because
libc memory cannot become a Go `[]byte`. The core already had the
caller-buffer entry point (`pbw_encode_to`), so nothing was missing: the
binding was choosing the expensive shape. Measured, that shape cost 52 ns of
a 220 ns 136 B encode (below); a first arithmetic estimate of ~95 ns was
wrong by 2x, which is why this was A/B'd instead of reasoned about.

### The change

- **core**: `pbw_encode_deterministic_to()`, the caller-buffer twin of
  `pbw_encode_deterministic()` (the delimited twin already existed). The three
  Go encode entry points now share one contract, so the binding does not need
  two different ownership stories.
- **binding**: `Encode()` / `EncodeDeterministic()` / `EncodeDelimited()` write
  into a Go-allocated slice with `pbw_encode_to` and friends. The capacity is
  `pbw_message_wire_size(msg) + PBW_ENCODE_SLACK` when the tree came from the
  wire, otherwise the capacity the last successful encode needed (an atomic
  field on `Message`), otherwise 256 — growing by doubling on
  `PBW_ERR_LIMIT`. No C `malloc`, no `pbw_free`, no copy back: the bytes the
  core writes *are* the result.
- The returned slice is a fresh, caller-owned `[]byte` exactly as before. The
  hint is advisory: too small costs a retry, too large allocates a few bytes
  more, and an atomic keeps concurrent `Encode` correct — so the API contract
  (fresh result, no retained memory, goroutine-safe) is unchanged, unlike
  reusing a context buffer (which invalidates the previous result) or handing
  the C buffer to Go (which cannot free it).

### A/B (5 alternating rounds, only `dynamic.go` differs, same library)

`ns` per encode, each round = one full harness run (best of 3 passes):

| tier | old | new | delta |
|---|---|---|---|
| 136 B | 242.1 / 211.1 / 220.5 / 236.6 / 216.3 | 167.8 / 162.7 / 166.7 / 180.0 / 175.2 | **-24%** (median 220.5 -> 167.8) |
| 174 B | 341.8 / 283.4 / 292.1 / 301.7 / 279.9 | 232.7 / 231.9 / 233.5 / 240.8 / 234.8 | **-20%** (median 292.1 -> 233.5) |
| 2469 B | 2485 / 2262 / 2220 / 2203 / 2306 | 2043 / 2010 / 1895 / 1952 / 2025 | **-11%** (median 2262 -> 2010) |
| 74913 B | 54168 / 54554 / 55171 (two rounds load-spiked) | 51026 / 51213 / 51503 | -6% (best round) |

New side wins every tier in every round. Same-run ratio against protobuf-go
(the harness times both sides in one process, so this is the cell to read):
136 B encode 0.94x -> **1.28x**, 174 B 0.94x -> **1.22x**, 2469 B 1.85x ->
2.19x, 74913 B 2.35x -> 2.45x (neutral, inside the tier's noise).

### Rejected in the same pass

- **`pbw_message_encoded_size()` before every encode** (size, then write):
  flips the small cells just as well, but at 74 KB the extra walk (~45 us) on
  top of the write walk doubles that tier, turning 2.4x into ~1.3x. A size
  walk is only affordable when it replaces a *copy*, never when it replaces
  nothing.
- **Reusing one scratch buffer for the result** (a field on `Message`): the
  returned `[]byte` must stay the caller's, so the only reusable buffer is a
  separate one, which means copying back into a fresh result — i.e. keeping
  the copy that this change removed.
- **Returning `unsafe.Slice` over the C buffer**: libc memory cannot be handed
  to the Go GC, and a finalizer does not make it safe.

### Cost and edges

- A cold message encodes with a 256-byte first attempt and grows by doubling,
  so a *one-shot* encode of a large hand-built message can walk up to twice
  (normally much less: a failed attempt stops as soon as the buffer is full).
  A decoded tree starts from its exact wire length, so the common
  decode-then-encode path never doubles.
- After a doubling the result is copied into an exact-size slice, so a caller
  never pins a 2x array; a steady-state encode returns `buf[:n]` with at most
  `PBW_ENCODE_SLACK` bytes of slack and no copy at all.
- Verified by `tests/test_pbwire.c:test_encode_to` (deterministic twin: bytes
  equal to `pbw_encode_deterministic`, 2-byte-short buffer is
  `PBW_ERR_LIMIT`, NULL args rejected), `make go-test` (byte equality and
  round trips over every tier), `make difftest` and `make test`.
