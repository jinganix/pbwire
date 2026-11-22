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
| 7 | split `encodeInto`: repeated-field branch and unknown-fields tail extracted to helpers, `fields`/`has`/`num`/`ref` hoisted to locals (the opposite of #5) | neutral (larger shapes 0.99-1.04x across runs; the 136 B row flipped sign with the oracle) | the singular loop is not smaller in any way the JIT can use: same loads per field, plus two cold call boundaries. Measured on a different, heavily loaded box, so ratios only, no ns/op. |
| 8 | object dispatch for the numeric singular path: static `NumWriter` lambda table indexed by kind instead of the `switch` (proxy for a `MethodHandle`/explicit-arity dispatch) | encode down on every shape, both runs: const 0.91x twice, varied 0.96-0.98x, 136 B 0.81-0.85x | a megamorphic call site costs more than the branch-predicted tableswitch. A faithful `MethodHandle` version would need a per-field constant call site for the JIT to fold, which runtime-built field definitions cannot provide without generating classes - so this stands in for that direction too. Same loaded-box caveat as #7. |

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
Elixir's xlarge decode 0.99x (closed later: see the one-pass struct build
below). The tables keep the strict `>= 1.0x` rule the
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

## Cell: Elixir xlarge decode (0.99x) — kept

BENCHMARKS.md listed `pbwire-elixir` 56.3 MB/s against `elixir-protobuf` 56.8
on the xlarge tier, the only Elixir cell under 1.0x, while every smaller tier
was 1.67-1.82x. That shape (worse as the payload grows) is the signature of a
per-message cost, not a per-byte one: the binding decoded the NIF's record
tuples and then walked them a second time to build the struct.

### Where the time went

`Pbwire.from_tuple/2` ran `Tuple.to_list` + `Enum.zip(fields, values)` +
`Enum.map` + `pairs ++ [__unknown_fields__: []]` + `struct(mod, pairs)` per
message — five passes and four intermediate lists, on every decoded message,
nested ones included (3001 of them at xlarge). The NIF side was already
one-pass; the Elixir side was not.

### The change

The schema spec now carries two per-position tables built once at load
(`keys`: the struct key per field, `flds`: the field metadata per field), and
`from_tuple` indexes them while walking the record once:

```elixir
defp from_tuple(%{mod: mod, keys: keys, flds: flds} = spec, tuple) do
  m = min(tuple_size(flds), tuple_size(tuple) - 1)
  struct(mod, tuple_pairs(spec, tuple, 0, m, keys, flds, []))
end
```

`struct/2` stays (the generated module's other defaults — `__protobuf__`,
`__pb_extensions__` — must survive), and a record shorter than the field table
keeps the struct defaults for the fields it does not carry, exactly like the
`Enum.zip` this replaces.

### A/B

Same machine, same harness (`make elixir-bench`), one run per side; the
baseline column did not move, which is what makes the comparison
`elixir-protobuf` 56.8 -> 57.2 MB/s (noise) against pbwire 56.3 -> 104.5 MB/s:

| tier | before (decode) | after (decode) | x baseline after |
|---|---|---|---|
| 136 B | 48.2 | 102.2 | 1.83x |
| 174 B | 99.7 | 105.0 | 1.81x |
| 2469 B | 56.1 | 109.9 | 2.02x |
| 74913 B | 56.3 (0.99x) | 104.5 | 1.83x |

Encode is untouched (it builds the tuple directly and was already 2.4-2.9x).
Verified by `make elixir-test` (30 tests incl. the generated-model suite) and
`mix format --check-formatted`.

## Rejected: Java "no trailing copy" and JVM decode allocation trims

Both are real wins on paper and both measured inside the host's noise band,
so they are not in the tree. The harness record (`Bench.java`) times a built
tree through six `Runnable`s at one call site; the codec's own floor is
1.1-3.3 GB/s. Two probes were built for this round:

- a two-classloader probe (`TimedCodec`, one JVM, both builds, interleaved
  rounds, `ThreadMXBean.getCurrentThreadCpuTime`), which needs the result
  stored in a static or escape analysis scalar-replaces the arrays and
  under-measures by 3x;
- a CPU-time copy of `Bench` (`BenchCpu`, same lambda shapes as the table),
  which is load-proof but still ±1.5-3% per row at 174 B.

What was measured (min over 5-9 interleaved rounds, CPU time, both sides):

| candidate | result |
|---|---|
| encode: size the workspace buffer from the last result, hand it out with no trim copy (`copyOut`) | 174 B 1.00-1.01x, 2469 B 1.00x, 74913 B 1.01x (the 75 KB copy costs ~1%: this machine's memcpy is ~100 GB/s) |
| decode: presence as a bitmap inside `num[]` instead of a `boolean[]`, shared empty `ref`/`oneofCase` arrays, `Limits.defaults()` singleton | 174 B 1.006-1.011x; the same build's untouched encode row moved 1.8-3.4% in the same runs |

Conclusions kept from the round: the trailing-copy removal is worth ~1% on
every tier, not the ~5% the byte count suggests; the allocation trims are
worth ~1% at 174 B; and both are below this host's floor (load average 16 on
14 cores from unrelated processes, matching the 2026-11-14 re-run round
above). Java's xlarge encode also measures 1.15x on this host against the
0.95x in the table, i.e. that cell is baseline-run-dependent. One change from
the round *did* survive: `Limits.defaults()` returns a shared immutable
instance instead of allocating one per decode (no behaviour, no risk,
one allocation per call removed).

## Round: the xlarge tier (Java 0.95x, Rust 0.98x in the tables)

BENCHMARKS.md marks two xlarge encode cells below 1.0x, and the historical
tables show the xlarge encode column used to lead by more (0.1.8: Java
1025.8 MB/s 1.15x, Rust 1596.5 MB/s 1.08-1.84x; 0.1.11: 1061.4 / 1398.4).
Cross-release comparisons in that file come from different sessions, so this
round asked the question directly: **is there a code regression at xlarge,
and does the tier still lead?**

### Is there a regression? No.

`git worktree` at 0.1.11 (`fb65ed0`) built its own `libpbwire.a`; the current
`bench/bench.cc` was compiled against both libraries, and the two binaries
were alternated on the same host, min-of-N:

| tier | 0.1.11 lib | HEAD lib | HEAD/0.1.11 |
|---|---|---|---|
| xlarge encode | 57966.8 ns | 55951.9 ns | **1.036x faster** |
| large encode | 2270.1 ns | 2117.9 ns | 1.072x |
| xlarge decode | 133918.5 ns | 137161.1 ns | 0.976x |
| small encode | 75.8 ns | 78.5 ns | 0.966x |

So the core did not regress at xlarge: HEAD is ~3.6% faster there than the
release the old tables were proud of. The xlarge numbers in BENCHMARKS.md
moved because their *baselines* did (Java's protobuf-java column 976.1 ->
1002.2 with the same jar, prost 1364.5 -> 1333.0), i.e. a quieter host for
the baseline side.

### Does the tier still lead? On this host, yes.

Same-session runs against the real baselines (host at load 10-16 from
unrelated Gradle/node processes, so absolute MB/s are 1.5-2x below the
tables; only the ratio in the same run is meaningful):

| binding | ours | baseline | x baseline |
|---|---|---|---|
| core `pbw_ctx` | 1.36-2.2 GB/s (encode) | protoc C++ 0.61-0.77 GB/s | 1.5-2.2x |
| Java | 965 MB/s | protobuf-java 835 MB/s | **1.15x** |
| Scala | 997.7 MB/s | ScalaPB 737.9 MB/s | **1.35x** |
| Rust | 851.9 MB/s | prost 768.9 MB/s | **1.11x** |

Both ❌ cells are therefore stale rather than regressions; they need a
re-run on an idle host before the table is rewritten (pasting these
load-affected absolute numbers would corrupt the same-machine tables).

### Where the xlarge encode time goes

A local sampler run (a copy of `bench.cc` that loops only the ctx xlarge
encode, sampled with `sample(1)`; the copy lives outside the repo) put 89%
of samples inside `encode_message` itself (the field loop and its inlined
writes), 9.5% in `_platform_memmove`/`DYLD-STUB$$memcpy` leaves and 2%
elsewhere. The copy leaves are the out-of-line `memcpy` calls for short
payloads: 3000 three-byte `tags` elements plus the packed length tags.

### Change kept: short payloads copy in line

`src/encode.c` grew `w_copy()`, used by `w_put` and by every bytes/string
len-tag/payload write: up to `PBW_COPY_INLINE` (16) bytes copy as unrolled
8/4/1-byte stores, longer payloads stay a `memcpy` call. Reads and writes
stay inside `[src, src+len)`, so nothing is over-read.

Verified by profiling the same harness again: the new build has **zero**
`_platform_memmove`/memcpy leaves in the xlarge encode (all samples fall in
`encode_message`), where the old build had 9.5%. Throughput, measured
paired (20 alternations, thread CPU time, min per side): median ratio
1.012, best 1.003, 11/20 rounds faster -- i.e. **about +1%**, well inside
this host's paired pair-to-pair noise (-35%..+34%). Kept because it removes
an out-of-line call per short payload with no downside and a 25-line diff,
not because the timer can see it.

`make test`, `make difftest` (byte equality against libprotobuf on every
case, 20000-input fuzz verdicts) and `make format-check` pass.

### What would actually move xlarge

The remaining 89% is the per-value field loop, which is what the ledger's
earlier attempts (varint windows, packed rewriting, buffer sizing) already
hit: repeated *unpacked* scalars pay one `write_element` dispatch per
element and nested messages pay one tag+placeholder reserve plus a
recursive field loop plus a length patch each. Both shapes are already
written as tight loops with hoisted cursors (see `write_packed`,
`w_varint_noreserve`), so a further win needs a different writer shape
(e.g. sizing children before writing them, at the cost of a second walk --
rejected in this file for the Go cell).

## Session 2026-11-14 (evening): the full matrix, and what it settled

Every harness was re-run on the same host (`make bench`, `make c-bench` ...
one at a time, 17:54-18:58 UTC, host carrying ~0.4-1.0 normalised load from
other tenants), and the tables in BENCHMARKS.md are those runs. Two cells flipped
because the *baselines* moved, not because of a code change: Java xlarge encode
0.95x -> 1.09x and Rust xlarge encode 0.98x -> 1.04x. In the same runs
`protobuf-java` fell 1002.2 -> 866.3 MB/s and prost rose 1333.0 -> 1398.4
MB/s while `pbwire-java` moved -1.4% and `pbwire-rust` +11.7% (the latter
carried the in-tree short-payload copy from the section above). Read both
flips as "at parity or above, inside the band", not as a win from a patch.

The rest of the sub-1.0x list did not move: Java/Kotlin/Clojure 136 B encode
(0.72x / 0.72x / 0.75x), Kotlin 174 B encode (0.80x, the same codec and bytes
swinging between 0.44x and 1.20x run to run), Scala 174 B decode (0.99x), core
plain 169 B encode (0.98x), Python 136 B decode (0.999x, a tie) and Julia
136 B decode (0.87x). Two attempts are worth recording.

### Cell: the JVM generated models, 136-174 B encode/decode

Root cause candidate: a generated model resolves every field by *name*,
`Message.set("str", ...)` / `Message.getString("str")`, which hashes the
string into `MessageDef.byName` once per field. A probe over the Java facade
(`LookupAB`, decode + 10 getters, alternating named vs numeric access) put
the named lookup at **3.07 ns per field**, so ~60 ns on a 22-field message.

Attempt: make the generated models address fields by number (a compile-time
constant the generator already has) instead of by name -- Scala's case-class
`fill`/`load` and the Clojure interop `fill!`/`->clj`.

Result: **neutral, reverted.** The Scala A/B (two class directories, old and
new `matrix3.scala`, five alternating rounds of the 174 B tier through the
generated case class only) gave encode 424 -> 415 ns (median, -2%) and decode
709 -> 730 ns (+3%): the model's own `ty.newMessage()`, `Message` construction
and case-class build dominate, and the two rounds that made the change look
fast were JIT warm-up. The Clojure harness moved 84.0 -> 86.5 ns on 136 B
encode and 129.4 -> 122.6 ns on 174 B (-5%), i.e. inside its own band, because
Clojure's map walk is what costs there, not the field lookup. The 3 ns/field is
real in the Java probe, but the message it applies to is a *direct* DMsg walk
(the benchmark harness), not the generated model, so the win was never on the
measured path. The generator change is also ~5 regenerated files of churn for
an unmeasurable result, so it is not kept.

### Cell: Julia 136 B decode (0.87x), and a workspace path that helps

`pbw_decode` owns one arena per tree: one `malloc` and one `free` per call. A
raw-ccall probe (no binding wrapper) split the 136 B decode: **129.4 ns** with
one malloc/free pair, **108.2 ns** the same parse through `pbw_decode_ctx` +
`pbw_ctx_reset` on a reused context (-16%), and the binding's wrapper adds
nothing measurable (129.4 vs 130.1 ns).

Change kept: the Julia binding gained `Ctx`, `reset!` and `decode_in(ctx, ty,
wire)` -- the only binding without the core's reusable workspace (C, Go, Rust
and Erlang already expose it). In the harness that is **+13%** on 136 B
(1000.4 -> 1126.5 MB/s), +12% on 174 B, +2% on 2.5 KB, and **-15%** on 74 KB:
above the context's 1 MiB retain cap the arena drops its head chunk on every
reset, so the plain path's single wire-length-sized allocation wins there.
All four rows are in the Julia table; the plain `decode` row stays the
headline, and the binding does not switch paths behind the caller's back.

Nothing else was kept: the 136 B Julia cell is still 0.87x because the
remaining 21 ns to ProtoBuf.jl's pure-Julia generated decode is the min core
decode cost plus a downcall, and closing it needs a different decode shape
(not a tuning fix).

## Round: small-tier boundary cells, 2026-11-15

Aim: the remaining sub-1.0x cells, with generic changes only (no per-payload
path). Five changes were measured; three are kept, two are recorded as rejected
or reverted.

### Kept: `pbw_arena_reset` keeps the used chunk prefix (core)

The reset kept exactly one chunk (the head) and freed the rest, so a tree larger
than its largest single chunk re-malloc'd its chunks on every decode. The reset
now keeps the chain prefix that covers the previous `used` total, bounded by
`retain_cap`. C probe, `matrix3` x3000 (74913 B), `pbw_decode_ctx` in a loop,
best of 5 x 300:

| build | ns/op (ctx decode) |
|---|---|
| keep head only | 125893 |
| keep used prefix | 104493 |

The plain path on the same payload is 102196-105760 ns, so the workspace stops
losing to the one-shot decode (-17%, and the Julia `dec+ctx` xlarge row went
0.88x -> 1.08x).

**Bug found in the first version of this change and fixed before keeping it:**
the prefix loop could keep zero chunks (when `used` summed to 0, i.e. a second
`pbw_ctx_reset` with nothing allocated), and returned with `arena->head = NULL`
without freeing the chain -- every chunk leaked. Reproduced with a
allocator-counting probe (`reset` then `reset` then `pbw_ctx_free` left 1 block
live); fixed by always keeping the head chunk, and `test_ctx_reset_twice()` in
`tests/test_pbwire.c` now asserts the retained head survives an idle reset.

### Kept: `pbw_decode_ctx` resets a sealed context on entry (core, semantics)

Saves the second call in a `reset`/`decode` loop. Contract note, because it is a
behaviour change: **a decode into a context that already holds a sealed tree
now invalidates that tree**, exactly like an explicit reset, so a caller that
decodes twice into one context without resetting no longer gets two live trees.
Header (`pbw_decode_ctx`, `pbw_decode_delimited_ctx`), `docs/guide.md` and
`docs/internal` notes were updated to say so.

### Kept: no `ThreadLocal` reader in the Java decode (pure-Java codec)

`GenericCodec.decode` used a `ThreadLocal<Wire.Reader>` as a scratch buffer.
One use of the workspace is not a reuse pattern worth a ThreadLocal: a local
`new Wire.Reader()` is scalar-replaced by C2, so the lookup (and the escape) go
away. Measured on the JVM harness (174 B decode, matrix3): pbwire 235-248 ns
across runs, and the Scala 174 B cell moved 0.99x -> 0.98-1.00x, i.e. the codec
was never the gap there. Kept because it removes a shared-mutable scratch
object (the reader no longer has to be reset to `MAX_DEPTH` in a `finally`)
and costs nothing measurable.

### Kept: `Message` drops its `AtomicBoolean` (Java/Kotlin/Scala/Clojure)

`Message.root()` allocated a `AtomicBoolean`, shared with child views, only for
`close()`/`segment()`'s open check. The class is documented as one-thread-per-
message, so the flag is a plain `boolean` on the root and views read
`root.closed`. Verified with a classpath probe on the same payload: raw
`GenericCodec.decode` 233 ns, `MessageType.decode()` + `close()` 226 ns (the
wrapper is already gone after JIT, which is also why this does not "fix" a cell
by itself).

### Reverted: `wire_len` write-back after a successful `pbw_encode` (core)

`encode_alloc` recorded the produced size in `msg->wire_len` as the next call's
buffer hint. Measured on the 169 B/core tiers: 0.98x -> 1.02x in one run and
0.96x in the next, i.e. inside the noise band the ledger already documents
(0.94-1.04x), while `pbw_encode` takes a `const pbw_message *` and would start
writing to it -- a contract change (and a data race for a message shared read-
only between threads) for an unreliable 2%. Reverted before the tables were
repasted; the exact-start hint is the caller's job through `pbw_encode_to`/
`pbw_encode_ctx`, where the capacity is already owned.

### Rejected: larger `growLong`/`growObj` minimum in the Java codec

Started repeated arrays at 16 elements; a 4-element start saves zeroing on the
narrow tiers. Measured: pbwire-side numbers did not move (247.9 -> 247.1 ns at
174 B), and the one run that showed 1.03x had a baseline that swung 234 -> 300
ns, so the change bought nothing and was reverted.

## Kept: the encoder caches its buffer base and capacity (core)

`writer` held only `pbw_buf *buf`, so every field write did
`w->buf->data + w->pos`: two dependent loads, and the second one is reloaded
after each store to `w->pos` because the compiler cannot prove the store does
not alias `buf->data`. The writer now caches `base` and `limit` and refreshes
them in `w_grow`, the only place they change.

Measured on `make bench` (same machine, one run each side of the change, core
tiers, `ns/op`; bytes identical, gated by the harness):

| tier | op | before | after |
|---|---|---|---|
| 132 B | plain encode | 82.9 | 73.8 |
| 169 B | plain encode | 112.4 | 98.3 |
| 2755 B | plain encode | 2294.3 | 1992.9 |
| 85933 B | plain encode | 56676.6 | 53937.2 |
| 169 B | `pbw_ctx` encode | 96.2 | 87.6 |

That is 10-13% off every encode path that goes through `pbw_encode_buf`
(plain, `pbw_ctx`, `pbw_encode_to`, and every C-linked binding: Go, Rust, Node,
Ruby, Lua, PHP, Zig, Julia, R, embedded). It also closed the last core cell:
**plain 169 B encode 0.96x -> 1.10x** against generated C++. The decoder's
equivalent (hoisting `msg->values` / `msg->present` out of the loop) was not
attempted: the ledger's earlier presence-word hoist measured neutral, and the
decode hot loop hands `msg` to out-of-line helpers in a dozen places.

### The two cells this does not close

- **Julia 136 B plain decode (0.89x).** Attribution, one probe of the same
  136-byte payload: C `pbw_decode` + `pbw_message_free` 126.1 ns,
  `pbw_decode_ctx` (no allocator call) 107.4 ns, so the one arena
  `malloc`/`free` pair is 18.7 ns; the Julia wrapper adds 7.5 ns
  (133.6 measured end to end) and ProtoBuf.jl's whole decode is 118.8 ns. A
  zero-cost wrapper would still be 126.1 > 118.8, so the cell needs the
  allocator pair gone -- an ownership change, since `decode` promises a
  mutable tree with a per-call lifetime. Rejected: allocating the arena from
  Julia's own heap. A 2048-byte `Vector{UInt8}(undef, n)` costs **47.3 ns**
  per call on this machine, three times the `malloc`/`free` pair it would
  replace, so the scratch-buffer path is strictly worse. The binding's
  `decode_in` (reusable `Ctx`) is the allocation-free path and measures
  0.98x-1.02x.
- **Scala 174 B decode (0.98x).** The codec is not the gap: `GenericCodec.decode`
  alone is 235 ns, the binding's `decode()` + `close()` is 247, and the
  baseline (ScalaPB, not protobuf-java, which is 321 ns on the same bytes) is
  241-247 across runs. A classpath probe shows the `Message` wrapper is already
  eliminated by the JIT (226 vs 233 ns raw), so what is left is the DMsg array
  set plus the per-field dispatch, and the cell flips with the baseline's own
  run-to-run band.

## Rejected: JNI binding for Java (2026-11-16)

Question: would the Java binding be faster on top of the C core through JNI
than the current pure-Java codec? Answer: no, except large-payload encode.
Do not re-attempt without a new reason; the numbers below are the ceiling
a real JNI data binding would sit under.

### Probe

A minimal JNI harness over `build/libpbwire.a` (four native methods, same
`matrix3.proto` / same four tiers as `Bench.java`): `nativeInit` loads
`matrix3.desc` via `pbw_descriptor_set_load`; `nativeSetRef` decodes the
reference bytes once into a cached C tree per tier; `nativeEncode(tier)`
re-encodes the cached tree (`pbw_encode`, one `byte[]` out per call);
`nativeDecode(bytes)` runs `pbw_decode` + touches the field/presence counts +
`pbw_message_free` per call (one `byte[]` in, no Java objects created). Both
sides allocate per call and cross the boundary once per op; timing is the same
calibrate + 3-pass best-of interleaved loop as `Bench.java`, so machine drift
hits both sides equally. Byte parity gated first: JNI bytes are identical to
`GenericCodec` bytes on all four tiers.

The probe was throwaway when it produced this table; it now lives in
[`bindings/java/pbench/`](../bindings/java/pbench/README.md) and reruns with
`make pbench-java`, so this decision can be re-priced from numbers instead of
memory (the 2026-11-20 re-run reproduced the ordering; host load moves the
large/xlarge encode cell between ~1.4x and ~2.0x, so read the pbench README's
own table as one sample, not the value).

### Result (Apple M4 Pro, OpenJDK 26.0.1, same process)

| tier | encode pure-Java | encode JNI (C) | JNI/pure | decode pure-Java | decode JNI (C) | JNI/pure |
|---|---|---|---|---|---|---|
| small 136 B | 1606 MB/s (84.7 ns) | 865 MB/s (157.3 ns) | **0.54x** | 886 MB/s (153.6 ns) | 536 MB/s (253.7 ns) | **0.61x** |
| medium 174 B | 1360 MB/s (128.0 ns) | 947 MB/s (183.7 ns) | **0.70x** | 735 MB/s (236.7 ns) | 551 MB/s (315.6 ns) | **0.75x** |
| large 2469 B | 985 MB/s (2506.5 ns) | 1516 MB/s (1629.1 ns) | **1.54x** | 718 MB/s (3436.9 ns) | 629 MB/s (3927.5 ns) | **0.88x** |
| xlarge 74913 B | 924 MB/s (81111.0 ns) | 1557 MB/s (48107.6 ns) | **1.69x** | 746 MB/s (100430.4 ns) | 690 MB/s (108600.4 ns) | **0.92x** |

### Why this is the ceiling, not the floor

The probe favours JNI on purpose: encode reuses a prebuilt C tree (no
Java-object-to-C translation per call) and decode never materialises Java
objects (no C-tree-to-`DMsg` translation). A real JNI data binding pays both
translations on top of these numbers, so it measures strictly worse. Even
so, JNI loses both ops on small/medium (one JNI call +
`GetByteArrayElements` pin/copy dominates an 80-300 ns codec), wins encode
only on large/xlarge (the C single-pass writer pulls ahead once the payload
dominates the boundary), and loses decode everywhere (C decode eagerly
materialises the whole tree plus a `malloc`/`free` pair per call, while
`GenericCodec.decode` is lazy on message/string/bytes fields).

### Decision

Keep the pure-Java codec. It already leads `protobuf-java` on xlarge
(encode ~1.0x, decode ~1.7x in `bench.sh`), ships zero native libraries,
and cannot segfault the host JVM. A JNI port buys at most ~1.6x on
large/xlarge **encode only**, at the cost of per-platform `.dylib`/`.so`
distribution, a second codec to keep byte-identical, and host-crash risk.
The one workload where JNI would win -- a gateway that re-emits large
payloads without touching fields -- is a passthrough, not a data binding,
and should be built as that if it ever matters.

## Probe round: Go dynamic arms (issue #41)

The published Go rows used to time the dynamic handle API (`Encode()` on a
tree already living in C, `Decode()` into a handle nobody reads) next to a
static generated table codec, which is not what the library is: generated
models may carry data, never wire decisions, and the wire lives in one engine.

The static generated-table path was therefore deleted (`neutral`,
`purego`, `table`, and the codegen Meta-plus-loader split went with it). No
runtime is shipped yet: the two dynamic cores compete over the same
`EncodeTable`/`DecodeTable` engine — the C core over cgo
(`pbench/dync/dynamic`) and a pure-Go core with a runtime value tree
(`pbench/dyngo/codec`, `codec.DMsg`) — each in its own solution
directory next to its arm, and `runtime/` stays empty until they select the
final solution. Generated models are thin structs plus a metadata file of
field tables. The selection landed: `bindings/go/runtime/` ships as the
default engine, while the pure-Go one stays a candidate whose table engine
models the cshim cannot drive fall back to.

> 2026-11-21 addendum: superseded — the cshim runtime was retired and the
> shipped Go binding is now the pure-Go table engine end to end. The entry
> above records the selection experiment, not the current state.

`bindings/go/pbench/` now compares exactly the three dynamic shapes, each
behind the same native-in / native-out contract (`Marshal` from a caller-held
struct, `Unmarshal` to a fully materialized fresh struct), with struct<->tree
moves through shipped `ImportModel`/`ExportModel` converters only — no
test-only glue — interleaved in one process, median of 5 rounds, gated on
byte-identical output and a field-by-field struct compare before timing.
Ratios are one Apple M4 Pro run:

| tier | op | dync (C core) | dyngo (Go core) | cshim (Erlang-style) |
|---|---|---|---|---|
| 136 B | enc / dec | 0.10x / 0.12x | 0.32x / 0.31x | 1.61x / 1.41x |
| 174 B | enc / dec | 0.09x / 0.13x | 0.35x / 0.35x | 1.64x / 1.41x |
| 2469 B | enc / dec | 0.05x / 0.11x | 0.27x / 0.35x | 2.16x / 1.98x |
| 74913 B | enc / dec | 0.05x / 0.12x | 0.16x / 0.36x | 2.20x / 2.07x |

A real bug surfaced while making the thin-model benchmark honest: the shared
table's capacity hint remembered the largest encode (the xlarge gate tier),
so every later small encode allocated and zeroed ~75 KB per call — 5.6 us on
the 136 B tier that vanished once the hint memorized the last actual size
plus headroom instead of the capacity. One table serves every payload size of
its message, so a capacity memory is always the wrong memory.

The ordering is the whole finding: a per-field copy into any tree — a C tree
through cgo worst, the Go value tree better but still per field — costs far
more than reading the struct where it already is. The Erlang shape (a
binding-local C file reading Go memory through a once-measured layout table)
is the shape to copy when a binding must be both native-object-end-to-end
and fast.

2026-11-20 baseline (dyngo fast-path work, Task 1): tree-path `dyngo` arm
re-measured on this host, median of 5 interleaved rounds — 136 B enc/dec
0.33x/0.32x, 174 B 0.35x/0.35x, 2469 B 0.27x/0.35x, 74913 B 0.22x/0.33x
against protobuf-go. A `dyngo-direct` gate arm (`EncodeModel`/`DecodeModel`
straight on the struct, no `DMsg` tree) is added and gated byte-identical;
its timed row lands in Task 4.

2026-11-20 after (Tasks 2-4): `dyngo` row now times the direct path with
the offset fast path live (generated `Off/Has/Count/Elem` + `unsafe` reads
on encode, scalar stores on decode). Same host, median of 5 rounds —
136 B enc/dec 1.39x/0.85x, 174 B 1.53x/0.87x, 2469 B 1.55x/1.11x,
74913 B 1.50x/1.13x, all at 1 alloc like the oracle. The jump has two
parts: dropping the struct<->tree copies (the tree arm stays as the
dynamic-path reference, still ~0.2-0.3x) and the offset reads replacing
per-field closures. Still on closures in v1: message rows, oneof,
`Foreign`, groups, explicit/pointer decode stores and repeated decode
(pointer stores skip the write barrier on purpose); `DMsg` unknown
retention is untouched, verified by the existing `codec` unknown tests.

### Recorded earlier answers (kept for the numbers, not as shipping plans)

- A language-neutral bulk table plus one cgo call caps encode at ~0.45x: the
  public core API has no bulk value/list constructor, so the per-row
  `pbw_value_new_*` + `pbw_message_set_field` pair cannot be batched away
  (~13.45 ns per present value against the cshim peer). Neutral core and a
  competitive native-object path are mutually exclusive.
- Straight-line per-message Go code reached ~2.0x encode / ~2.0x decode at
  xlarge, but it is a second wire implementation pinned only by the parity
  gate. That ceiling is what the must-stay-dynamic rule costs; it is not a
  shipping option.
- The generated table codec (one data table per message over the shared
  engine) reached ~1.4-1.6x encode and ~0.9-1.3x decode with the plan gate
  per call. That path was still static generated code, and it was removed
  once the dynamic Go core existed; the engine it drove is the one both
  dynamic cores use today.

## Attempt (Go thin model): exact-size presize, vtprotobuf SizeVT idea — rejected

vtprotobuf's serial wins come from three things: per-message straight-line
code (already tried and rejected above: ~2.0x but a second wire
implementation), exact `SizeVT` presizing (one allocation, no headroom, no
length-prefix backpatch), and message pooling. Only the second is expressible
without static codegen, so it was tried on the shared table engine: floor the
256-byte minimum to the cold path only and cut the `setCapHint` headroom
from `+64` to `+8`, so a warmed 136 B encode asks for 144 instead of 200
(256 size class). Two full `bench.sh` runs, base then candidate, same host
(Apple M4 Pro, darwin/arm64, go1.27.4); the in-process protobuf-go side is
the drift control. Reverted.

| tier | op | base pbwire | candidate pbwire | x vs oracle base -> cand |
|---|---|---|---|---|
| 136 B | encode | 163.5 ns, 256 B/op, 1 alloc | 155.9 ns, 144 B/op, 1 alloc | 1.36x -> 1.36x (ns neutral, B/op parity reached) |
| 136 B | encode-par | 84.1 ns | 56.5 ns | 0.75x -> 1.04x (suspect, see below) |
| 174 B | encode | 198.2 ns, 256 B/op, 1 alloc | 236.9 ns, 432 B/op, 2 allocs | 1.45x -> 1.20x, regression |
| 174 B | encode-par | 96.4 ns, 1 alloc | 139.9 ns, 2 allocs | 0.88x -> 0.61x, regression |
| 2469 B / 74913 B | encode | 2655.2 / 77778.1 ns | 2683.9 / 76974.3 ns | 1.74x -> 1.75x / 1.78x -> 1.76x, neutral |
| all tiers | decode | untouched path, moves inside its own spread | - | neutral |

Why it fails: one table serves every payload size of its message, so the
hint is last-size, not this-size. Small warms it to 144; medium (174 B) no
longer fits and regrows (2 allocs, 432 B/op, +20% ns). The old `+64` was
not slack but cross-size tolerance: 136 -> 174 is +38. The encode-par
"win" on small is not claimed: both sides moved absolutely (oracle 63.5
-> 59.0 ns) and par rows are load-sensitive, while the medium regression
carries a discrete alloc-count change, which is mechanism, not noise.
The real vtprotobuf shape — exact size computed per encode, not remembered
from the last one — would need a `SizeTable` walk per encode; that walk
costs more than the ~112 zeroed bytes it saves on small tiers (same reason
the C ledger entry rejects `pbw_message_encoded_size` before every encode:
+45 us walk at 74 KB).

Second half of the same idea, measured without a code change: a temporary
`Encoder.Begin/Data/End` microbench put the length-prefix backpatch at
~3.9 ns for a 15 B (inner-like, single-byte length, no shift) versus
~15.0 ns for a 200 B (packed-100-like, two-byte length, memmove) payload.
The shift premium is ~11 ns per large packed field, ~0.8% of the 2469 B
encode — below what a size-walk replacement would cost. No change made,
probe deleted.

## Kept (Go thin model): buf-local row staging fixes encode-par scaling

The serial fast path was already offset reads (BENCHMARKS.md Go section),
but every row still stored through `e.Buf` per append: with the Encoder
heap-escaped, each store is a GC write barrier, per element on packed
repeated loops. Parallel encodes scaled ~1.3x on 14 cores while protobuf-go
scaled ~5x in the same process (in-run ratios 0.32x-0.62x on encode-par),
which is the gap the Go table publishes. The `setCapHint` shared-write half
was already gated (stores only past a 2x size move); this change is the
barrier half: `encodeFast`/`encodeFastRepeated` stage the row in a
stack-local `buf` with one writeback, and the packed length-prefix helpers
live once as `beginLen`/`endLen`, which `Encoder.Begin`/`End` now delegate
to (no second implementation to drift). No wire change, no generated-code
change: same primitives, fewer stores.

A/B on a loaded host (Apple M4 Pro, go1.27.4): a best-of probe alternating
base/candidate builds, then one full `bench.sh` per side with the in-process
protobuf-go side as the drift control. Ratios are same-process pbwire/oracle.

| tier | op | base x oracle | candidate x oracle |
|---|---|---|---|
| 136 B | encode | 1.59x | 1.32x |
| 136 B | encode-par | 0.62x | 0.82x |
| 174 B | encode | 1.23x | 1.36x |
| 174 B | encode-par | 0.37x | 1.31x |
| 2469 B | encode | 1.28x | 1.79x |
| 2469 B | encode-par | 0.32x | 2.08x |
| 74913 B | encode | 1.08x | 1.68x |
| 74913 B | encode-par | 0.58x | 1.26x |

The probe agreed on the parallel direction in all three rounds
(encpar-x3000 -42%/-41%/-60%, encpar-x0 -47%/-31% before a box spike flipped
round 3 to +15%); serial small-tier deltas sit inside this host's own
control drift (the untouched decode path moved +-18% between back-to-back
runs), so the serial column is read as neutral-to-positive, not as a claim.
Allocs and B/op are unchanged on every cell (1 alloc encode; same bytes, so
the parity gate still binds). Decode is untouched by this change; its par
rows in the candidate run are scheduler noise, not a regression (identical
code, sequential windows).

Kept because encode-par wins in every round on both harnesses and nothing
else moves outside the box's noise band. The Go table in BENCHMARKS.md still
shows the pre-fix par rows: they need one quiet-host `bench.sh` re-run
before repasting (this host's oracle absolute moved 2x between the two sides
of the pair, so the pair's ratios are the finding, not its MB/s).

Safety half: `runtime/concurrency_test.go` pins the contract the par rows
rely on — shared table with per-goroutine messages, one read-only message
shared across Marshal goroutines, and fresh-target concurrent decodes, all
byte-checked — and the full `runtime` suite passes under `-race`.

## Kept (Go thin model): cold-only encode floor — small B/op 256 -> 208

The 256-byte presize floor bound every steady-state small/medium encode
(`capHint` learns 200/238 for the 136/174 B tiers but the floor overrode it),
while the oracle allocates 144/176. Under parallel encodes that is ~1.8x the
allocation-byte rate and the GC load that follows — the signature matches the
cells: serial small encode wins (1.2-1.3x, CPU-bound, fewer dispatches) while
small encode-par loses (0.66-0.69x, throughput-bound). This is the other half
of the rejected vtprotobuf presize above, not a repeat of it: that attempt cut
the `+64` headroom to `+8`, and the medium tier stopped fitting its hint (2
allocs, 432 B/op, +20% ns). This change keeps `+64` untouched and only stops
applying the floor once the table has learned a size (`capHint() == 0` is
exactly "never encoded"): cold one-shot encodes still get one 256 B
allocation, large tiers never see the floor either way, and small/medium
steady state allocates the learned 200/238 (208/238 size classes). No new
constants, no size classes, no tier knowledge — it removes a binding case.
Applied to all three presize sites (`EncodeModel` plus the two `DMsg`
encodes, same principle).

Two bench.sh pairs, base then candidate back-to-back, in-run protobuf-go
ratios (absolute ns moves 3x between runs on this box; only same-process
ratios read):

| tier | op | pair 1 base -> cand | pair 2 base -> cand |
|---|---|---|---|
| 136 B | encode | 1.19x -> 1.34x | 1.05x -> 3.44x (oracle-side noise inflates this one) |
| 136 B | encode-par | 0.69x -> 1.06x | 0.66x -> 0.81x |
| 174 B | encode | 1.20x -> 1.36x | 1.13x -> 1.54x |
| 174 B | encode-par | 1.04x -> 1.31x | 0.44x -> 0.94x |
| 2469 B | encode / encode-par | 1.87x / 0.48x -> 1.75x / 2.08x | 1.72x / 0.75x -> 1.71x / 0.93x |
| 74913 B | encode / encode-par | 1.65x / 1.12x -> 2.04x / 0.95x (xlarge phase spiked 4x, ratios only) | 1.66x / 1.02x -> 1.62x / 1.46x |

Allocs stay 1/1 on every encode cell; B/op drops 256 -> 208 on small/medium
and is unchanged elsewhere. Parity gate byte-identical on all four runs.
Decode cells move +-noise on the untouched path (the control), including one
candidate-run medium decode-par outlier both sides' spreads disown.

Kept because the paired encode-par direction wins in 7 of 8 comparisons (the
one reversal is the spiked xlarge phase), serial never moves
the wrong way, and the change is zero-cost by construction (same code path,
smaller buffer, still a single allocation — verified, not assumed). What it
does not do: small encode-par is narrowed, not reliably flipped (0.81x vs
1.06x across pairs — the oracle's own spread decides the 1.0 line on this
box), and the decode-par cells are untouched with no generic lever left (the
arena upper bound is structural; a decode-side size hint would put a shared
atomic write on a path that is currently read-only-shared, i.e. reintroduce
the contention the barrier work just removed). Those cells need a quiet-host
re-run before any further claim, same as the table re-run already noted
above.

## Kept (Go thin model): exact-size hints — small/medium B/op to size-class parity

Follow-up to the rejected `+8` attempt above: that cut failed because the
hint is last-size and medium (+38 over small) no longer fit, regrowing every
encode. Since then the overflow path (`setCapHintOverflow` /
`setArenaHintOverflow`) relearns the moment a buffer outgrows its presize, so
cross-size moves cost one realloc per new max instead of one per encode —
and the slack has no job left. This change drops the `+16` on both encode and
decode hints (store exact `n`), plus the `Grow(h + 16)` overshoot, so steady
payloads allocate exactly what they use.

Same host, `make go-bench` before -> after (medians; full log in
`results/bench-go-20261122T*.json`):

| tier | encode B/op | decode B/op | encode x | decode x | encode-par x | decode-par x |
|---|---|---|---|---|---|---|
| 136 B | 160 -> 144 (== oracle) | 404/388 -> 368 | 1.32x -> 1.38x | tie | loss -> 1.02x ✅ | 1.06x ✅ |
| 174 B | 192 -> 176 (== oracle) | 480 | 1.50x -> 1.47x | 1.01x ✅ | 1.10x ✅ | 1.05x ✅ |
| 2469 B | unchanged 2688 | 14208 | 1.72x ✅ | 1.21x ✅ | tie 1.00x | 0.96x ❌ |
| 74913 B | unchanged 81920 | 499011 -> 458050 | 1.70x ✅ | 1.11x ✅ | 1.40x ✅ | 0.93x ❌ |

Mechanism, not noise: 136 + 16 = 152 lands Go size class 160 while the
oracle's 136 lands 144; 174 + 16 = 190 lands 192 vs 176. Exact sizing moves
both small tiers onto the oracle's size class with the same single
allocation (verified 1/1 allocs on every tier — the overflow path absorbs
the small -> medium -> large -> xlarge ratchet, one realloc per tier, then
steady). Serial decode B/op drops the same way (arena `Grow` exact).

Not claimed: the two large/xlarge decode-par gaps (0.96x/0.93x). The same
work serially wins 1.21x/1.11x with 7 GC-isolated passes, and repeats flip
the par verdict both ways (rerun: large decode-par 5513 vs 6138 win; 5x
focused probe: 1 win, 4 losses by 2–8%), while each par pass is an ~18 ms
window on a 13-user box. No further generic lever identified: per-op traffic
(B/op) is already at or below the oracle on every cell and alloc counts are
lower everywhere, so any remaining delta is scheduler placement, not work.
Re-verdict belongs on a quiet host, not in more code.

## Reverted (Go thin model): precomputed on-wire tags per Field

Hypothesis: the profile put inlined `AppendVarint` at 12.6% flat on xlarge
encode, so baking each row's tag bytes at `newTable` (Packed rows bake LEN,
groups stay tagless on the old path) and appending them should remove a
shift/or/call per emitted field (~6k tags on xlarge). Implemented
(`tag[5]/tagLen` on Field, `appendTag`/`beginTag` helpers, ~110 swapped
sites), unit test first, full suite + 4-tier parity gate green.

Bench (same box, loaded both runs; oracle drift only +2..+8%):

| tier | encode x before -> after |
|---|---|
| 136 B | 1.38x -> 1.16x |
| 174 B | 1.47x -> 1.32x |
| 2469 B | 1.72x -> 1.46x |
| 74913 B | 1.70x -> 1.47x |

Ours slowed +18..+25% absolute on every encode cell while the untouched
decode cells held their ratios (the control). Mechanism: a variable-length
slice append (`f.tag[:f.tagLen]`) goes through a runtime growslice call per
tag, while the old path for 1-byte tags inlined to shift/or plus a plain
byte append — and the hot xlarge tags (nums 17-22) are 2 bytes, so every
repeated element paid it. Precompute only pays if the store is as cheap as
what it replaces; here it replaced an already-inlined two-instruction
sequence with a call. Fully reverted (semantic WT restoration per Kind,
parity gate re-verified), no remnants.

## Kept (Go thin model): exact presize for packed fixed-width + bool runs

decodeRepeated packed runs grew geometrically (appendFastPacked: one
load/store-back, but N growths). Fixed widths know their count exactly
(len/4, len/8) and conformant bool runs are one byte each (len/1); varint
kinds stay untouched (count <= bytes would over-allocate up to 10x on
ordinary multi-byte values, and exactness for all conformant inputs is the
rule). `appendFastPacked` takes a width (0 = unknown) and grows once when
short; hostile truncation still fails on the short read inside the loop, and
the cap never exceeds input length plus the old length.

TDD: `pack_presize_test.go` asserts cap == len == 100 for a 100-elem
fixed64 run (failed before: geometric cap 128). Full suite + 4-tier parity
gate green. Deterministic bench columns (load-independent):

| tier | decode allocs | decode B/op |
|---|---|---|
| large (100x) | 133 -> 131 | 14208 -> 14169 |
| xlarge (3000x) | 3057 -> 3049 | 458050 -> 452797 (-5.2 KB abandoned arrays) |

Small/medium unchanged (no fixed/bool packed runs beyond 0-1 elems; the
medium +2 allocs/+88 B in the same loaded run contradicts the mechanism —
the change cannot add an allocation by construction — and is background
MemStats noise on a load-59 box; re-verify on a quiet host). ns verdict
pending quiet box; the xlarge B/op drop is fewer mark-phase bytes, which is
the parallel-scaling lever. Reverted-then-kept note: this run also
confirmed the E-TAG revert restored encode to wins on all tiers
(1.63x/1.29x/1.31x/1.37x under thrash).

## Implemented, verdict pending (Go thin model): two-byte varint fast path

`Decoder.Varint` and `Decoder.Next` had only the one-byte fast path; the
loop handled everything else. The 2-byte forms (values 128..16383: packed
Nums/Inner values, tags for field numbers 16-2047 — i.e. the repeated
bulk) now go through one shared `consumeVarint2` helper with the same
accept set as the loops (non-minimal two-byte forms included, verified by
`varint2_test.go`; Next keeps its 0/6/7/MaxFieldNumber checks, which still
apply). TDD: helper unit test first (failed before impl), full suite +
4-tier parity gate green after. ns verdict needs a quiet box (load 60+ at
write time); the added cost on the 1-byte majority is one predictable
branch, so the plausible range is small-win to neutral — revert on neutral.
Considered and deliberately not done in the same pass: unrolling 3+ byte
forms (diminishing, branchy), touching ConsumeVarint/ConsumeLen (cold paths).

## Deliberately not attempted (Go thin model), with reasons

- Packed-varint presize (bound = byte length): over-allocates up to 10x on
  ordinary multi-byte values, not just hostile input. Bounded-sizes
  principle. Fixed widths and bool presize instead (exact for all
  conformant inputs).
- Arena / message pooling: no release hook in the ownership contract.
- Per-message straight-line codegen: the repo rules call an independently
  encoding generated model a bug.
- Repeated-string cross-occurrence batching: occurrences arrive one per
  decodeField call, so the per-element Bytes/Valid/copy work is
  irreducible at this level; only header traffic would batch (analysis
  showed ~2-3 ns/element, not worth the new path).
- tag precompute (E-TAG): tried, reverted above.
- Batch-allocated repeated submessages: the only remaining structural cost
  (~5-10 ns/element closure + interface), but needs a codegen shape change
  (e.g. a batch-alloc hook + regen). Revisit only if the quiet-box bench
  still shows inner-heavy decode gaps after presize + varint2 land.

## Reverted (Go thin model): exact presize for packed fixed-width runs

TDD started clean: `pack_presize_test.go` failed before, passed after,
full suite + 4-tier parity gate green. Deterministic bench columns showed
xlarge decode 3057 -> 3049 allocs and B/op -5.2 KB (abandoned growth
arrays). But medium decode moved 10 -> 12 allocs / 472 -> 560 B/op and
stayed there across every control, which broke a previously-green cell
(medium B/op vs oracle 488). Reverted; medium is back at exactly 10/472.

Bisect (each step min-of-20 AllocsPerRun, load-independent): bool width
forced 0 -> still 12; varint2 neutered -> still 12; full presize removal ->
10; re-adding a *no-op* helper call -> 12; signature-only 5-arg with dead
param -> 10. So the trigger is any real call inside appendFastPacked, and
`-gcflags=-m` names it: `moved to heap: sub` at every packed site. The
extra body pushes appendFastPacked over the inline budget, so it no longer
inlines into decodeRepeated and the address-taken `sub Decoder` escapes
(+1 alloc per packed run: medium has Nums + Flags = +2, exactly the
delta). The growth savings (-11 on xlarge) dwarf it on big tiers, which is
why xlarge still improved net, but on one-element runs the presize saves
nothing while the escape costs one: net +1 per run. The growth-array saving
is real but the escape tax wipes the small tiers, including a B/op cell
that used to win.

Lesson, verified not reasoned: an extra parameter or call in a hot generic
helper can flip inlining and force an address-taken local to escape, and
AllocsPerRun catches it while code review cannot. Any revival of this idea
must keep appendFastPacked inlinable (prove with `-m`: no `moved to heap:
sub`) or pass the run cursor without taking its address. Candidates that
do neither — presize-at-call-site with 14 duplications, fire-only-when-big
thresholds — were considered and dropped as tuning smells or complexity
without a cleanliness win.

## Kept (Go thin model): two-byte varint fast path in Next + Varint

`consumeVarint2` handles values 128..16383 (packed Nums/Inner values,
two-byte tags for field numbers 16-2047 — the repeated bulk) with one
boundary decision instead of the counting loop; accept set identical to
the slow paths, pinned by `varint2_test.go` (including non-minimal forms).
Serial decode moved up everywhere with no other change in the tree:
small 1.00x -> 1.02x, medium 1.01x -> 1.09x (spreads no longer overlap),
large 1.21x -> 1.21x, xlarge 1.11x -> 1.15x (spreads no longer overlap).
Zero allocation delta by construction (pure arithmetic on the existing
slice header); medium AllocsPerRun stays 10, xlarge 3057. Kept.

## Kept (Go thin model): 2-byte tag fast path (fastTag2)

The 1-byte fastTag lane covered only singular field numbers 1-15, so
every repeated field (nums/tags/inners/flags: numbers 17-22, all 2-byte
tags), the singular submessage (16) and both optionals (20-21) paid one
`Next` call plus one `lookup` per occurrence — ~6000 of each per xlarge
decode, ~8 per medium. `Next` (cost 279) and `Bytes` (164) both exceed the
80 inline budget permanently (three split shapes tried and reverted in
earlier entries), so the only way to remove the call is to not make it:
resolve the tag inline. New per-table columns `fastTag2[256]*Field` /
`fastTag2b[256]byte`, keyed by tag first byte with the second byte as the
verifier (built in `newTable`, first-byte collisions stay empty and keep
the generic path, so a hit proves number and wire type exactly like
`fastTag`). The loop prologue parses the 2-byte tag inline and falls
through to the *shared* repeated/DecFast/decodeField lanes — zero body
duplication, zero new calls on the path (`lastTag` rides along exactly
like `Next` sets it, pinned by an unknown-retention test).

TDD: `fast_tag2_internal_test.go` (population + collision poison, failed
to build before), `fast_tag2_test.go` (mixed packed/unpacked parity,
unknown retention, first-byte collision fallback, dangling-tag
truncation — green on the generic path before, green after). `-m`
confirms no new call on any hit path. Deterministic columns unchanged by
construction (same bodies downstream): allocs 5/10/117/3035 and B/op at
or below the oracle on every tier, full suite + `-race` green.

Three clean same-host runs (load ~14), medians, fastTag2 tree vs the
pre-change run-F single:

| tier | decode before | decode after (3-run median) |
|---|---|---|
| small | 1.38x | 1.40x |
| medium | 1.25x | 1.26x |
| large | 1.33x | 1.37x |
| xlarge | 1.20x | 1.27x |

Directionally up everywhere (+0.01..+0.07); the single-run spread on this
box is ±0.04-0.07, so the table now reports the medians, not a draw.
Serial stands at 6/8 over the 1.3x bar (medium 1.26x, xlarge 1.27x remain).

## Implemented, verdict pending (Go thin model): 1-byte length fast path

Same pattern as `varintByte`: `Decoder.bytesFast1` handles only 1-byte
lengths inline (bounds-check, load, slice, advance — `-m` confirms it
inlines), wired into the 6 hot Len sites (repeated string/bytes/message
lanes plus decodeField's singular string/bytes/message; packed-run sites
skipped — bulk lengths are 2-byte, an always-miss branch there is pure
cost). Accept set mirrors `Bytes`' first branch exactly, pinned by
`bytes_fast1_internal_test.go` (hit/zero/multibyte-miss/truncated-miss/
empty-miss, failed to build before); fallback to `Bytes` preserves errors.
Full suite + `-race` green, allocs/B-op identical (same work, fewer calls).

No usable ns evidence yet: the two runs attempted with it in-tree both
spiked box-wide (load 61, oracle side 2-3x slower — discarded per the
ledger rule, not pasted). It rides in the tree on mechanism (`-m` clean,
~6000 removed calls/decode at bulk, zero behavior delta) until a quiet
box can arbitrate; revert on neutral. If it lands at the estimated
+2-4%, medium (1.26x) and xlarge (1.27x) both clear 1.3x.
