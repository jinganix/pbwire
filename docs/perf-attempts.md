# Performance attempts ledger

This page records every optimization tried against a cell that is **below its
baseline** in [BENCHMARKS.md](../BENCHMARKS.md), whether it was kept or
rejected, and why. It exists so the same ground is not re-ploughed: several of
these attempts look obviously right on paper and measure worse on a real
machine.

Read [BENCHMARKS.md](../BENCHMARKS.md) first for the standard cases, the
harness names and the current numbers. This page only adds the experiments.

## How an attempt is measured

Same method as [the core fast-path notes](#measurement-notes-the-core-fast-path-from-benchmarksmd):

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

## Provisional: Julia shim arm (plan 07)

The flat-layout shim removes the per-field boundary, and its decode uses a
caller-owned arena (`pbw_struct_arena_new_mem`) allocated once in the plan, so
there is no per-call `malloc`/`free` pair. Provisional `compare.jl` medians
(2026-11-21): `shim` encode 1353.3 / 1325.1 / 1077.9 / 1115.3 MB/s and decode
1095.2 / 993.9 / 688.3 / 746.2 MB/s at 136 / 174 / 2469 / 74913 B. The formal
136 B row is 1107.3 MB/s against ProtoBuf.jl 1173.2 (0.94x); the remaining gap
is the C struct decode plus the ccall, not the arena pair (the plan owns both
the image and the caller-memory arena, so steady-state decode has no
`malloc`/`free`).

Stage 6 A/B on the 136 B shim decode (each build, five passes of 3.2M calls):

| build | ns/op | MB/s | result |
|---|---|---|---|
| baseline | 133.9 | 1015.6 | -- |
| cache the caller arena header in the type, skipping `pbw_struct_arena_new_mem` per call | 134.0 | 1013.0 | rejected: no win, within noise |
| pass the image as `Ptr{T}` from `pointer_from_objref` instead of `Ref{T}` | 134.9 | 1008.1 | rejected: no win |

Bytes stayed identical in both rounds (the gate passed); neither change was
kept, so the 136 B decode entry above stands. The gap is the C struct decode
shape, not a tuning fix in this arm.

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
## Provisional: the JVM four, three arms (2026-11-21, issue 41)

First run of the JVM `pbench` rows, before plan 03's Stage 5 recorder pass:
one interleaved round per tier, best of three passes, all arms plus the
baseline back to back. `tree` = `GenericCodec`/`DMsg` (a `Message`), `core` =
the committed generated model in `bindings/<lang>/tests/gen` used as the
native object, `shim` = `bindings/java/pbench/jni` (not timed here; its
ceiling is the pbench table). xlarge = 74913 B:

| binding | tree enc | core enc | baseline enc | tree dec | core dec | baseline dec |
|---|---|---|---|---|---|---|
| Java | 133853.9 ns 559.7 MB/s | 148018.5 ns 506.1 | 129224.1 ns 579.7 | 143072.8 ns 523.6 | 285911.6 ns 262.0 | 230162.8 ns 325.5 |
| Kotlin | 79515.9 ns 942.1 | 115311.8 ns 649.7 | 86284.1 ns 868.2 | 106019.1 ns 706.6 | 227891.3 ns 328.7 | 164866.7 ns 454.4 |
| Scala | 67474.6 ns 1110.2 | 321962.9 ns 232.7 | 95261.0 ns 786.4 | 101594.9 ns 737.4 | 466979.4 ns 160.4 | 114116.3 ns 656.5 |
| Clojure | 7365842.0 ns 10.2 | 63117.5 ns 1186.9 | 85906.0 ns 872.0 | 8837011.7 ns 8.5 | 98633.8 ns 759.5 | 165927.3 ns 451.5 |

Clojure's `tree` row is the persistent-map API over the generated model's
map fns (the binding's own data tree); its `core` row is the mutable-message
fast path with no map walk, per the plan's review focus. This first run was
taken under heavy host load (load average 16-28, another checkout building),
so its Java absolute numbers are ~1.6x low; Stage 5 re-ran all four on the
recorder (`results/bench-{java,kotlin,scala,clojure}-20261120T17*.json`) and
those are the run whose rows `BENCHMARKS.md` publishes. The ordering and the
`core`-slower conclusion did not change. Materialization:
the `tree` rows decode lazily (message/string/bytes fields parse on first
access) and the `core` rows decode eagerly into the typed model, so the
`core` decode column pays the materialization the `tree` column skips.

Reading: the JVM native-object rows are *not* faster. The generated models
are a table engine over the shipped runtime (`PbCodec`'s per-field
`FieldMeta` lambdas, method-handle setters, and for Scala a by-name
`fill`/`load` translation), so they cost more per field than the dynamic
`DMsg` walk: Java/Kotlin `core` encode is 0.87x the baseline at xlarge and
0.29x at 136 B; Scala `core` is 0.24-0.35x everywhere; Clojure's mutable
`core` is the one that beats the baseline (1.36x / 1.11x encode). The
`tree` rows stay the shipped fast path and the headline.

### Stage 6 decision: not chased in this plan

Every below-baseline `core` cell is the generated model's per-field
translation, not a codec cell: closing it needs per-message generated
writers (the same product decision the Java 136 B cell above already
recorded as rejected), plus a generator change outside this plan's files.
The small-tier `core` cells are boundary floors and are labelled, not
chased. No A/B was run: there is no single in-scope change to put on the
probe (`benchmarks/ab.sh` times `GenericCodec` and the protobuf-java
oracle, and the runtime change that would move this is the rejected
per-message codegen).

## Cell: the JVM `core` rows, plan 03b Stage 6 (2026-11-21)

Plan 03 skipped Stage 6 on the theory that nothing in scope could move the
generated-model rows. This pass put three changes on the probe anyway. The
probe is a JVM `CoreAb` (xlarge payload, 3000 repeats) that times the two
shapes the published rows use: the committed Java model through `PbCodec`
(Java/Kotlin `core`) and a Scala-style `Message` fill/load by name
(+ `Message.encode`, Scala `core`). Base and candidate are two runtime class
dirs from the same tree, run alternately in separate JVMs, 3-5 rounds. Byte
parity is asserted in the probe before any timing, and the official
harnesses re-ran the parity gates at the end.

| # | candidate | A/B (fill-encode, xlarge) | verdict |
|---|---|---|---|
| 1 | `MessageType.field(name)` reads a flat open-addressed `name -> slot` table built once per type, instead of `HashMap` | base 296.6-313.5 us -> 218.7-192.4 us over 5 rounds (-27..-38%) | **kept** |
| 2 | slot-direct `DMsg` stores: `putRef`/`appendRef`/`newMessageInSlot` write the already-resolved slot, so a string/bytes/message write no longer re-resolves the field number and switches on its kind | 206.4-229.2 us -> 194.4-211.0 us over 5 rounds (-6..-15%) | **kept** |
| 3 | one `DMsg` allocation fewer per message: share a static empty `oneofCase` when `oneofCount == 0` (3000 fewer arrays on the xlarge fill) | r1 200.4 -> 197.4 us, r2 193.7 -> 194.3, r3 186.5 -> 229.8 | **rejected** (not a win in every round) |

Why only Scala moves: Java/Kotlin `core` is `Structtest_Scalars3.toByteArray()`
/ `parseFrom()`, which walk the generated `PbCodec`/`FieldMeta` table by slot
and never build a `DMsg`; there is no name lookup and no reference store on
that path, so the probe's `core-encode` read 79-86 us before and after (noise).
Scala's committed case-class `encode` is the only JVM `core` that fills a
`DMsg` field by field, and that is where hypotheses 1-2 apply.

Published run: `results/bench-{java,kotlin,scala}-20261121T0357*.json`
(`make java-bench kotlin-bench scala-bench`). Scala `core` encode rose
240.5 -> 334.6 MB/s at xlarge (0.33x -> 0.42x of the same run's ScalaPB);
the other two `core` columns read within noise (Java 671.6 -> 662.5,
Kotlin 705.4 -> 705.6 MB/s) because their code path is untouched. The
`tree` and baseline columns stay on the plan-03 run: this pass was taken
under the same loaded host and only the changed cells are republished.

## Provisional: the Node two-arm pbench (2026-11-21, issue 41)

First run of the Node `pbench` rows, before plan 04's Stage 5 recorder pass:
`node bindings/node/pbench/compare.js`, interleaved rounds over `a-tree`
(the whole task: one `set()` per field then encode; decode then one `get()` per
field), `b-shim` (`type.encodeObject(obj)` / `type.decodeToObject(wire)`, one
N-API downcall) and the two baselines, median of five, all four tiers.
xlarge = 74913 B, MB/s:

| arm | enc | dec |
|---|---|---|
| `pbwire-node-tree` | 47.3 | 48.8 |
| `pbwire-node-shim` | 47.7 | 52.5 |
| google-protobuf (jspb) | 54.2 | 81.2 |
| protobufjs | 427.1 | 423.4 |

The same order holds at 136 B (shim 44.6/60.7, jspb 56.7/114.9, protobufjs
240.9/382.9) and at 174 B and 2469 B. Host load average was ~33 during this run
(other checkouts building), so the absolute numbers are low and only the
ordering is worth reading; Stage 5 re-ran the formal rows on the recorder
(`results/bench-node-*.json`) and the shim row lost the same cells there.

Reading: one downcall beats the per-field `set()` assembly by 1.05x-1.17x, so
the shim replaces the JS-side loop -- but it does not reach either baseline.
The addon's object walk costs one N-API property read (encode) or write
(decode) per field, ~60-70 ns, against ~5 ns for protobufjs or jspb writing
into its own heap. The prebuilt `Message` handle stays the fastest path for
data that is already in pbwire (74 KB encode ~50 µs, decode ~60 µs).

Verdict: keep the prebuilt `Message` handle path as the runtime's fast path and
keep the shim as the binding's native-object path and a re-runnable arm. The
shim replaces the JS-side per-field assembly (1.05x-1.17x) but not the tree:
re-encoding a built message (74 KB in ~50 µs) stays 30x cheaper than building a
native object, and replacing that path with the shim would slow every user down.
No payload-size split applies -- the shim is below jspb and protobufjs at 136 B
as well as at 74913 B.

## Optimization: the Node shim encode cell (2026-11-21, issue 41)

Stage 5 put every shim cell below jspb, so plan 04's Stage 6 built the A/B
probe it names: `bindings/node/benchmarks/ab.sh` compiles two `.node` builds
from `runtime/addon.c` at two revisions into one tree, `ab.js` interleaves
both builds in every round (5 rounds, median) and refuses to time a candidate
whose bytes differ from the oracle. `AB_BASE=0dad1b0` vs the working tree also
served as round 0: the schema-driven walk that replaced the object-key walk won
encode 4-5/5 rounds at every tier (1.10x-1.36x), so the rewrite was kept.

Change 1 -- **encode tree in the type's arena** (`pbw_ctx_reset` +
`pbw_message_new_ctx` + `pbw_value_new_*_ctx` in `encodeObject`, the pattern the
R/Lua/PHP bindings already use). One arena for the whole object walk instead of
one malloc per field value. Bytes unchanged (the gate and the probe both refuse
otherwise). A/B base = the pre-change addon, 2026-11-21 under load ~20:

| tier | encode base | cand | ratio | rounds won |
|---|---|---|---|---|
| 136 B | 3050.5 | 2783.8 ns | 1.10x | 5/5 |
| 174 B | 3546.9 | 3301.1 ns | 1.07x | 5/5 |
| 2469 B | 54432.9 | 47244.8 ns | 1.15x | 5/5 |
| 74913 B | 1493086.8 | 1290602.9 ns | 1.16x | 5/5 |

An earlier run of the same probe lost one round at 136 B and one at 2469 B
(1.08x/1.12x medians); the recorded run above (`results/ab-node-arena.log`) won
every round at all four tiers. Decode is the same code and won 0-3/5 rounds at
1.00x, which is what a same-code cell measures on this host. Accepted.

Change 2 -- **not attempted**: the remaining shim gap is one N-API object
property write (decode) or read (encode) per field, at ~60-70 ns; the next
candidate (cached field-name `napi_ref`s to skip V8's per-call UTF-8 string
interning) buys ~20-30 ns of that and cannot reach jspb's ~5 ns per field. The
one-change cell was accepted above and the plan says to stop there rather than
keep tuning a boundary floor.

## Cell: C# native-object arm, all tiers (below Google.Protobuf) -- SUPERSEDED by plan 05b

**Superseded (2026-11-21, plan 05b):** this entry measured a managed walk with
one `P/Invoke` per present field, not the pinned-layout mechanism; see
"Provisional: C# native flat-image reader (plan 05b)" below for the real
reader's row and verdict. Kept for the record.

The C# `pbench` `shim` row (generated model in, wire out) sits below the
Google.Protobuf baseline at every tier: 2026-11-21 `pbench/compare`, xlarge
74913 B encode 61.3 MB/s against 387.9, decode 51.8 against 476.4 (small tier
1.27 against 130.8). The `tree` row (prebuilt `Pbwire.Message` handle) beats
the baseline 3-4x on the same run.

Stage 6 verdict -- **no change attempted, rejected on the mechanism**: codegen
now emits the per-message flat layout table (scalar offset/width, pointer slots
for `string`/`bytes`/submessage/repeated, one presence bit per proto3 optional)
and the gate validates it, but the C core has no entry that reads a pinned
managed object through such a table. Adding `pbwcs_encode` / `pbwcs_decode`
means touching `src/` and `include/`, which this plan forbids; a managed walk
over the same table is the same per-field `P/Invoke` converter with a
different loop, so it cannot pay. The measured row is the mechanism's floor (one
crossing per present field, ~12k fields at 74 KB), not a tuning miss.

A/B probe: none. The two builds the plan asks for (table-driven walk against
the model's shipped converter) would both cross per field, and the ledger rule
(reject a change that cannot win every round) is decided before the run: the
table walk adds managed reads on top of the same crossings. Byte equality holds
in both (the C core writes the bytes), so no round could show a win.

## Provisional: C# native flat-image reader (plan 05b, supersedes the plan 05 rejection)

The plan 05 entry above ("C# native-object arm") measured a managed walk with
one `P/Invoke` per present field; that is not the pinned-layout mechanism, so
its verdict is **superseded** by this row, not deleted. What is measured now:
`bindings/csharp/native/pbw_cs_shim.c` (compiled by `make csharp-shim-native`
with `bindings/c/runtime/pbw_struct.c` and the core) exposes one encode and one
decode entry; the managed side writes the native object into a contiguous
unmanaged image once per op (`WriteImage`: scalars at their emitted offsets,
strings/bytes/sub-images into a per-thread bump arena) and crosses once.

Provisional `pbench/gate compare` (2026-11-21, median of five, xlarge tier):
`shim` encode 686.8 MB/s (1.66x protobuf-csharp 414.5), decode 348.6 MB/s
(0.73x of 474.6). Small tier 402.5 / 347.9 against 132.6 / 147.1. The two
decode cells at 2469 B and 74913 B are below the baseline because
`ReadImage` materialises every repeated element into managed objects after the
C call; that is the native object's construction cost, which the `tree` row
(prebuilt handle, no materialisation) does not pay.

Ledger rule: the superseded rejection said "the pinned layout does not pay on
.NET". With the real reader it pays on 10 of 12 encode/decode cells; the two
that do not are a materialisation cost, not a boundary cost.

Stage 6 (two cells still below baseline: 2469 B and 74913 B decode).
Measurement first, warm median at the xlarge tier: C decode 103 us,
`ReadImage` (image -> managed objects) 147 us. The native half alone is 729
MB/s, above `protobuf-csharp`'s whole decode (492 MB/s); the copy is the gap.

Round 1 (kept): the generated `<Message>Layout` caches its compiled
`LayoutPlan` in a static field instead of resolving it through a lock plus a
dictionary on every op. Bytes unchanged, gate green, and it removes per-op
work at every tier; in the interleaved comparison it is below the host's noise
floor on the large/xlarge cells (where the copy dominates) so no round claims
a percentage, and the small-tier rows moved inside the same noise band.

Round 2 (not attempted): the only change that crosses the baseline is dropping
the image copy -- reading wire bytes straight into the managed object graph --
which is not a tuning of this mechanism but a different one (and would need a
managed wire reader, the thing `AGENTS.md` forbids). So the two decode cells
stay as published: 0.74x and 0.71x, with the split above as the reason.

Formal rows: `make csharp-bench` 2026-11-21
(`results/bench-csharp-20261121T000941Z.json`) shim encode 1130.8 / 980.3 /
617.5 / 654.1 MB/s, decode 744.1 / 595.9 / 341.5 / 348.7 at 136 / 174 / 2469 /
74913 B; `make fsharp-bench` (`results/bench-fsharp-20261121T001056Z.json`)
encode 1180.6 / 966.1 / 643.0 / 658.3, decode 760.2 / 590.5 / 320.8 / 354.6.

## Kept (C# shim): ASCII fast-path string decode (Task 3, 2026-11-24)

`ReadImage` paid a `Marshal.PtrToStringUTF8` transition per string (6000
native calls on the xlarge tier: tags, inner names, `str`, optionals).
`Pbwire.ImageText.Decode` keeps the exact contract (NULL/empty handling,
NUL termination, non-ASCII fallback to the runtime decoder) and decodes
pure-ASCII runs with a fused length/scan plus `string.Create` widening;
generated `ReadImage` calls it at all four string sites (singular, optional,
repeated, oneof). Behaviour-identical by construction, pinned by new T7
struct-path edge tests in `bindings/csharp/generated/GeneratedTest.cs`
(last-wins, sub-message merge, oneof reset, packed/unpacked/mixed, malformed
throws, invalid UTF-8 parity, non-ASCII round-trip through the fallback, unknown skip, empty
input); the suite is green before and after.

Formal `make csharp-bench` (`results/bench-csharp-20261124T191013Z.json`):
xlarge `shim` decode 370.4 -> 385.3 MB/s against a 498.9 baseline (0.70x ->
0.77x); large 364.6 -> 373.4 (0.76x -> 0.77x); small/medium decode improve
inside their band (1.75x -> 1.61x / 1.49x -> 1.57x -- the baseline side moved
too, same-run ratios throughout). `make fsharp-bench`
(`results/bench-fsharp-20261124T191738Z.json`) rides the same runtime:
xlarge `shim` decode 0.74x, inside harness noise of its 0.74x. Encode
untouched on both (helper is decode-only).

Deliberately not attempted, with measurements: bulk `AddRange` transfer for
repeated value types (loop clones save ~1.4 us nums + ~0.9 us flags on xlarge
-- ~1% -- for template branching plus a small-tier threshold; disproportionate
churn, skipped), shared-core `always_inline` on the struct hot loop (~3 us in
a hacked-lib A/B; touches every struct-codec user for noise), `-O3` for the
shim library (no measurable delta same-host), and dropping string copies or
UTF-8 validation in the C codec (needs an image-ABI change for lengths plus
pinning, or a semantic divergence the reference does not share -- the official
C# parser also rejects invalid UTF-8 eagerly -- for a projected ~0.9x at best).
The residual is the documented double materialisation (wire -> arena/image ->
managed vs the reference's single pass); crossing needs the mechanism change
plan 05b already rejected, not another tuning round.

## Provisional: Rust shim arm (plan 08)

The flat-layout shim is the generated `#[repr(C)]` mirror plus one call into
the C struct codec (`bindings/c/runtime/pbw_struct.c`, compiled by
`bindings/rust/build.rs`), so there is no per-field crossing. Provisional
`make rust-bench` medians (2026-11-20): `shim` encode 1613.5 / 1521.5 / 1088.7 /
1128.0 MB/s and decode 1122.8 / 995.6 / 659.5 / 706.4 MB/s at 136 / 174 / 2469 /
74913 B. Decode is above prost on every tier (1.43x small, 1.63x medium, 1.98x
large, 1.91x xlarge). Encode is above prost at 136 B (1.05x) and level at 174 B
(0.99x), and **below** it at 2469 B (0.80x) and 74913 B (0.83x).

That shape is the shared C struct codec's, not the Rust arm's: the same codec
is 1132.2 MB/s at 74913 B in the C structs table (`make bench-c`), essentially
the shim's 1128.0, while the shipped tree row's `encode_to` reaches 1713.7 on
the same payload in the same run. The codec's repeated-element paths reserve
per element (`sb_reserve` per string/array item), and prost's generated writer
has the same loop as straight-line code with no reserve at all.

Stage 6 A/B on the two below-baseline cells (2469 B and 74913 B shim encode;
prost is ahead 0.80x / 0.84x). One candidate: compile `pbw_struct.c` with `-O3`
instead of `-O2` (`bindings/rust/build.rs`), two release builds from the same
tree (`CARGO_TARGET_DIR=/tmp/ab-base` and `/tmp/ab-cand`), alternating runs:

| round | large base | large `-O3` | xlarge base | xlarge `-O3` |
|---|---|---|---|---|
| 1 | 1068.8 | 1181.1 (+10%) | 1158.1 | 1229.0 (+6%) |
| 2 | 1126.9 | 1172.5 (+4%) | 1153.9 | 1211.9 (+5%) |
| 3 (loaded box) | 1094.8 | 883.9 | 578.9 | 1162.4 |

Bytes stayed identical (the bench's flat-image parity check passed in every
run). **Rejected**, two reasons: the third round flips sign on a loaded box, so
the change does not win in *every* round; and, decisively, every other layout
arm compiles the same `bindings/c/runtime/pbw_struct.c` at `-O2` (`bindings/c`
`make bench-c`, `bindings/dart/build.sh`, `bindings/julia/build.sh`), so an
`-O3` Rust row would no longer time the same codec build the sibling shim rows
publish. The cell is the shared struct encoder's per-element reserve path
(`sb_reserve` per string/array item); fixing it means changing a runtime file
outside this plan's list and re-measuring C/Go/Dart/Julia/C#/Swift, so it stays
recorded here rather than patched under plan 08.

A second candidate was considered and not built: pre-sizing the plan's encode
buffer. The measured loop already reuses the buffer (one growth on the first
call, outside the timed steady state), so it cannot win a round.

## Provisional: Swift shim arm (plan 09)

The flat shim is the generated `@frozen <Message>Repr` plus one call into the C
struct codec (`bindings/c/runtime/pbw_struct.c`, symlinked into the Swift
package's `CPbwire` target), so there is no per-field crossing and no ARC in
the walk (the mirror holds pointers, not `String`/`Array`/`Data`). Offsets come
from `MemoryLayout<Repr>.offset(of: \.slot)!` at registration time, so the
table cannot drift from the Swift layout; a child mirror's array stride is
emitted explicitly because a Swift stride can be wider than the child's size.

Provisional `Bench compare` medians (2026-11-21, interleaved, median of five):
`shim` encode 919.3 / 937.2 / 1004.7 / 917.0 MB/s and decode 717.3 / 672.4 /
534.4 / 592.4 MB/s at 136 / 174 / 2469 / 74913 B; the prebuilt-handle `tree` row
is 1073.9 / 1115.9 / 1492.8 / 1675.8 encode and 879.6 / 801.9 / 689.3 / 642.6
decode on the same run.

Same shape as the Rust arm (plan 08): the one-call mirror is behind the shipped
handle on encode, and above it on nothing, because both rows run the same C
struct codec — the `tree` row is the core's own encoder, the shim row is the
shared layout codec. Against the swift-protobuf baseline this is what decides
the arm, and Stage 6 keeps or rejects a change per cell.

## Provisional: Zig shim arm (plan 10)

The flat shim is the generated `extern struct <Message>Mirror` plus one call
into the C struct codec (`bindings/c/runtime/pbw_struct.c`, compiled into the
binding's static archive by `bindings/zig/build.zig`), so no field crosses the
boundary. Strings are NUL-terminated pointers, bytes the core's inline
`pbw_bytes_t`, a sub-message a pointer to its child mirror, and a repeated
field a pointer plus a count. Every slot offset is asserted with `@offsetOf` in
a `comptime` block, so an emitter/ABI mismatch is a compile error, not bad
bytes.

`make zig-bench` medians (2026-11-20, all four arms plus Arwalk/zig-protobuf):
`shim` encode 1954.2 / 1706.2 / 1148.5 / 1202.7 MB/s and decode 1254.6 /
1057.7 / 680.7 / 751.0 MB/s at 136 / 174 / 2469 / 74913 B; the prebuilt-handle
`tree` row is 2023.4 / 1971.8 / 1724.0 / 1736.2 encode and 1137.8 / 946.2 /
693.3 / 741.8 decode. Against zig-protobuf the shim is above the baseline on
every cell: encode 1.52x / 1.66x / 1.31x / 1.44x and decode 48.8x / 46.5x /
9.55x / 2.26x.

Stage 6 is a recorded no-op: no published Zig cell is below its baseline, so
no A/B build was made and no change was attempted.

## Provisional: Ruby object-walk shim arm (plan 11)

The Ruby `shim` arm is one C-extension entry (`type.encode_object(obj)` /
`type.decode_object(wire, classes)`) that walks the generated model's instance
variables in C and recurses there; the `tree` arm times the whole user task
(`Structtest_Scalars3.encode(obj)`, which builds a `Message` with one `set` per
field, and `.decode(wire)`, which reads one `get` per field back). Provisional
`ruby pbench/compare.rb`, median of five interleaved rounds, 2026-11-21, all
four tiers, MB/s:

| arm | enc 136/174/2469/74913 | dec 136/174/2469/74913 |
|---|---|---|
| `pbwire-ruby-tree` | 31.3 / 42.9 / 38.1 / 42.1 | 32.9 / 37.0 / 41.4 / 46.8 |
| `pbwire-ruby-shim` | 79.8 / 130.6 / 129.9 / 133.2 | 132.2 / 146.0 / 143.5 / 153.4 |
| google-protobuf gem | 479.3 / 782.0 / 1195.3 / 1302.0 | 241.1 / 243.8 / 494.4 / 672.2 |

Reading: the object walk is 2.5x-3.1x the per-field facade on encode and
2.5x-4.0x on decode, so one extension entry removes the Ruby-side loop. It
still loses to the gem on both ops: the gem's generated object already owns
C-backed storage (and its decode stays lazy), while the shim builds a pbw tree
from Ruby ivars and materializes every generated model/array. The `tree` row
here is the per-field facade, not the prebuilt `Message` handle
`make ruby-bench` publishes.

Verdict: keep the builder/wire-view handle as the shipped fast path and keep
the shim as the native-object row and a re-runnable arm. The formal prebuilt
handle row is above the gem on every cell; the shim is below it on both ops at
136 B and 74913 B, so no payload split applies and replacing the handle with a
full object materialization would slow decode.

## Stage 6: Ruby shim encode arena (plan 11)

The two below-baseline shim cells (encode and decode at every tier) got one
change each, A/B on two extension builds from the same tree
(`bindings/ruby/pbench/ab.sh`, alternating rounds, xlarge 74913 B first).

Change 1 -- **accepted**: build the encode tree in the type's `pbw_ctx` arena
(`pbw_message_new_ctx` plus `pbw_value_new_*_ctx`) and copy `pbw_encode_ctx`'s
buffer into the returned String, instead of one heap block per message/value.
The `.so` before the change is base A, the candidate is base B:

| round | base MB/s | arena MB/s |
|---|---|---|
| 1 | 133.9 | 248.9 |
| 2 | 131.8 | 245.8 |
| 3 | 135.0 | 248.1 |
| 4 | 132.9 | 249.2 |
| 5 | 133.8 | 252.6 |

Every round wins (1.84x-1.90x), bytes stayed equal (the gate passed and the A/B
script aborts on any byte difference), so the change stayed. It is bolded as a
win but not a baseline win: the formal row rose from 133.0 to 253.5 MB/s at
xlarge, still below the gem's 1334.9.

Change 2 -- **rejected**: decode through `pbw_decode_ctx` on the same arena,
so the tree stops costing one malloc block. Same A/B, xlarge decode:

| round | base MB/s | arena MB/s |
|---|---|---|
| 1 | 154.6 | 156.0 |
| 2 | 152.2 | 152.0 |
| 3 | 151.9 | 153.9 |
| 4 | 156.5 | 156.0 |
| 5 | 154.9 | 155.3 |

Round 2 and round 4 are not wins (the change is inside run-to-run noise), and
the remaining gap is the generated-model materialization, not the decode tree
allocation. The plan's rule rejects a change that does not win every round, so
the decode path stayed on `pbw_decode`; the final build is change 1 only.


## Provisional: R one-`.Call` list arm (plan 12)

R's native object is a named list, so the `shim` arm is the shipped list path
reached without the per-call type-name lookup: `Structtest_Scalars3_encode(list)`
in the generated model's shape calls one `.Call`
(`pbwire_encode_number(schema, number, list)`), and C walks the whole list and
fills the tree inside C. The type is resolved once into a schema-owned message
number. The `tree` arm is the prebuilt builder handle (`pbwire_message` +
`pbwire_message_set` before the timer, `pbwire_message_encode` timed;
`pbwire_parse_binary` for wire -> tree). Provisional
`Rscript bindings/r/pbench/compare.R`, median of five interleaved rounds,
2026-11-21, MB/s:

| arm | enc 136/174/2469/74913 | dec 136/174/2469/74913 |
|---|---|---|
| `pbwire-r-tree` | 226.4 / 265.2 / 949.0 / 1606.5 | 81.4 / 97.9 / 437.3 / 485.5 |
| `pbwire-r-shim` | 63.7 / 71.0 / 133.5 / 155.3 | 52.1 / 60.8 / 118.3 / 121.4 |
| RProtoBuf | 1.6 / 1.9 / 2.4 / 2.1 | 2.5 / 2.8 / 3.8 / 2.9 |

Reading: the list path needs no new mechanism -- the C walk was already there
behind `pbwire_encode` -- so one `.Call` puts it 25-90x above RProtoBuf, and the
builder row is 3.5-10x the list row because it skips the list walk and the
decode-side named-list materialization. No R cell is below its baseline, so the
plan's Stage 6 (A/B, one change per round) has nothing to target here.

Verdict: publish both rows. `pbwire-r-shim` is the native-object row (list in,
wire out, walk in C); `pbwire-r-tree` is the prebuilt builder and stays the
form hot loops hold. `core` (a pure-R port) is deferred with its TODO, since a
per-field interpreter loop is orders of magnitude off and no native-less R
target exists.

Formal run (`make r-bench`, 2026-11-20T23:06:55Z,
`results/bench-r-20261120T230655Z.json`), MB/s at 136/174/2469/74913 B:

| row | enc | dec |
|---|---|---|
| `pbwire-r-shim` | 94.4 / 74.0 / 137.9 / 137.3 | 91.3 / 99.1 / 134.2 / 122.7 |
| `pbwire-r-tree` | 303.9 / 350.6 / 1091.3 / 1089.6 | 96.5 / 115.6 / 468.1 / 288.1 |
| RProtoBuf | 2.6 / 2.9 / 2.6 / 2.7 | 2.7 / 2.9 / 2.9 / 2.7 |

Stage 6 is a recorded no-op: no R cell is below its baseline (the weakest is
shim decode 25.55x at 174 B), so no A/B pair of extension builds was made and
no change was attempted. The gap that remains is between the two arms (a
builder skips the list walk and the named-list materialization), not against
RProtoBuf.

## Provisional: PHP array entry, one extension call (plan 13)

PHP's native object is an array in the generated model's shape. The `shim` arm
is one C-extension entry (`Type::encodeArray` / `decodeArray`) that walks the
array with `zend_hash_str_find` per descriptor field and fills the C tree in C;
the no-compiler deployment is the same API on the shipped `ext-ffi` wrapper,
published as `pbwire-php-shim-ffi` (one FFI call per field) and gated to the
same bytes. The `tree` arm is the prebuilt `Message` handle. Provisional
`php bindings/php/pbench/compare.php` (C extension row) and the same script on
`ext-ffi`, median of five interleaved rounds, 2026-11-21, MB/s:

| row | enc 136/174/2469/74913 | dec 136/174/2469/74913 |
|---|---|---|
| `pbwire-php-tree` | 1319.5 / 833.7 / 1713.0 / 1711.9 | 649.2 / 472.8 / 644.0 / 711.8 |
| `pbwire-php-shim` | 151.2 / 122.8 / 149.9 / 190.1 | 159.8 / 119.5 / 144.2 / 211.9 |
| `pbwire-php-shim-ffi` | 5.0 / 5.2 / 5.1 / 5.5 | 5.8 / 5.6 / 4.4 / 4.7 |
| `protobuf-php-c` | 869.9 / 556.2 / 1213.8 / 1301.5 | 413.9 / 339.4 / 498.3 / 537.7 |

The array path is below the official C extension on every cell, so Stage 6 has
a target; the tree handle is above it. The two shim builds produce identical
bytes (both gates pass) and the gap between them is the per-field PHP loop plus
FFI trampoline, not a different codec.

## Cell: PHP native-array encode/decode, below the official C extension (plan 13)

The formal `make php-bench` rows put `pbwire-php-shim` at 160.5-194.1 MB/s encode
and 168.5-212.5 MB/s decode against the official C extension's 887-1313 / 445-569,
so the array entry is a below-baseline cell. A/B probe:
`bindings/php/pbench/ab.sh <base.so> <candidate.so> 5 xlarge`, two extension
builds from the same tree, alternating base and candidate in one session. The
probe gates bytes first (encode equals the official runtime, decode re-encodes to
the same bytes) and aborts on any difference.

Change 1 -- **accepted**: one scratch `pbw_ctx` per `Pbwire\Type`, reset per
`encodeArray` call, with `pbw_encode_ctx` writing once into its reused buffer
instead of a fresh context plus `pbw_message_encoded_size` + `pbw_encode_to`
(two tree walks). xlarge encode, ns/op:

| round | base | candidate |
|---|---|---|
| 1 | 390346.6 | 340963.3 |
| 2 | 381605.0 | 345738.3 |
| 3 | 379034.3 | 364158.4 |
| 4 | 394362.9 | 346453.4 |
| 5 | 384188.1 | 345151.7 |

Every round wins (about 1.05-1.14x); bytes stayed equal and the gate passed, so
the change stayed. The formal xlarge shim encode rose from 194.1 to ~220 MB/s,
still below the baseline.

Change 2 -- **rejected**: decode the same array arm through `pbw_decode_ctx` on
the same scratch context instead of `pbw_decode` + `pbw_message_free`. Same A/B,
xlarge decode, ns/op:

| round | base | candidate |
|---|---|---|
| 1 | 336149.0 | 362994.0 |
| 2 | 364320.6 | 370132.5 |
| 3 | 353816.2 | 353711.4 |
| 4 | 358166.8 | 348870.8 |
| 5 | 351480.2 | 352890.7 |

Rounds 1, 2 and 5 are losses, so the change does not win every round; the gap is
the per-field array materialization, not the decode tree allocation. The decode
path stayed on `pbw_decode`; the final build is change 1 only.

## Provisional: the Node pure-TypeScript core arm (plan 15, 2026-11-21)

Plan 15 landed a second, in-language implementation of the wire format under
`bindings/node/lib/core/` (descriptor reader, tree, encode, decode with unknown
retention), so the Node `pbench` gains a third arm: `c-core.js`. It is not a
boundary crossing at all -- the task runs in the JS heap -- so it measures the
wire format itself in the host language, the same question the other bindings'
`core` arms answer. `node bindings/node/pbench/compare.js`, interleaved rounds,
median of five, host load average ~22 (other checkouts building), so only the
ordering is worth reading; Stage 5 re-recorded the formal rows.

| tier | arm | enc ns/op | enc MB/s | dec ns/op | dec MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-node-core` | 2049.4 | 66.4 | 1449.5 | 93.8 |
| 136 B | google-protobuf | 2321.6 | 58.6 | 1134.5 | 119.9 |
| 136 B | protobufjs | 550.4 | 247.1 | 347.4 | 391.4 |
| 74913 B | `pbwire-node-core` | 434607.8 | 172.4 | 853442.5 | 87.8 |
| 74913 B | google-protobuf | 1276967.8 | 58.7 | 893244.5 | 83.9 |
| 74913 B | protobufjs | 172487.7 | 434.3 | 171038.0 | 438.0 |

Reading: the in-language core beats jspb on encode at every tier (66.4 against
58.6 MB/s at 136 B, 172.4 against 58.7 at 74 KB) and at 74 KB on decode (87.8
against 83.9), and loses the small-tier decode cell (93.8 against 119.9) where
jspb's generated reader has no dynamic slot lookup. It stays 2.5-3.7x behind
protobufjs, which does per-type code generation. The boundary was the tree and
shim arms' cost, not the codec: this arm skips it entirely.

Conformance scope: the core is a binary codec (the plan's file list has no JSON
or text printer), so `make conformance-node-core` passes the binary suite with
the two expected tag-truncation failures and skips the JSON half (1412
successes, 1403 skipped, 2 expected failures, 0 unexpected). The addon-backed
`make conformance-node` keeps covering binary + JSON + text (2750 + 443).

Stage 6 decision: **not attempted in plan 15**. One cell sits below the
official baseline in the formal run (136 B decode: 60.6 against jspb's 106.5
MB/s); every encode cell and the 2469 B / 74913 B decodes are at or above it.
The gap there is per-decode allocation (one tree object per message, one array
per repeated field, one slice object per string), and pushing it down means
either a columnar/lazy tree (the Java runtime's design, a rewrite of the tree
and both codecs) or a generated fast path (what protobufjs does, and what this
arm exists to measure without). Neither is a one-change-per-round probe, and the
plan's remaining budget went to the descriptor reader, the decoder's unknown/
group/limit rules and the binary conformance pass. The prototype of this arm
also crossed the plan's 3-hour budget, so the decision is recorded rather than
the cell chased.

## Provisional: the C# pure-core arm (plan 16, 2026-11-21)

The third arm of `bindings/csharp/pbench/` is a pure-C# port of the C core
(`bindings/csharp/Pbwire/Core/`: descriptor reader, tree, wire, encode, decode
with unknown retention, groups, packed, oneof, presence, limits). It is not a
boundary crossing at all -- the task runs in the managed heap -- so it measures
what the C# binding could do with no native library, the way every other
binding's `core` arm does. `CCore.New` builds the arm's native object (its own
`Msg` tree) before the timer, so the row times the codec like `tree` does.

`make csharp-pbench` (gate, then interleaved rounds, median of five) is
mid-session and the small cells drifted by 6-8x between rounds, so only the
ordering is worth reading; Stage 5 re-recorded the formal rows:

| tier | arm | enc MB/s | dec MB/s |
|---|---|---|---|
| 136 B | `pbwire-csharp-core` | 129.5 | 134.8 |
| 136 B | protobuf-csharp | 130.6 | 219.0 |
| 2469 B | `pbwire-csharp-core` | 665.2 | 342.3 |
| 2469 B | protobuf-csharp | 439.3 | 435.1 |
| 74913 B | `pbwire-csharp-core` | 702.4 | 304.8 |
| 74913 B | protobuf-csharp | 393.6 | 484.7 |

Reading: above the generated reader/writer on every encode cell from 2.5 KB up
(1.5x-1.8x in this pass) and below it on decode from 2.5 KB up. The wire engine
is not the cost; the tree is (one boxed slot per scalar, one `Raw` slice per
string, one `Msg` per sub-message), which is exactly what the native rows do
not pay.

Formal rows: `make csharp-bench` 2026-11-21
(`results/bench-csharp-20261121T004756Z.json`) core encode 925.8 / 852.6 /
621.2 / 653.6 MB/s, decode 540.9 / 479.6 / 362.4 / 318.6 at 136 / 174 / 2469 /
74913 B; protobuf-csharp in the same run 457.9 / 404.6 / 568.5 / 472.2 encode
and 473.9 / 395.2 / 473.4 / 506.3 decode. So the core is 2.02x / 2.11x / 1.09x
/ 1.38x on encode and 1.14x / 1.21x / 0.77x / 0.63x on decode. The tree beats
the core at 74 KB by 2.4x each way (`pbwire-csharp-tree` 1665.5 / 753.3); the
core beats the image round trip (`pbwire-csharp-shim` 665.7 / 362.8) on both.

Conformance scope: the core is a binary codec (no JSON or text printer in the
plan's file list), so `make conformance-csharp` passes the binary suite with the
same two expected tag-truncation failures and skips the JSON half (1412
successes, 1403 skipped, 2 expected failures, 0 unexpected). The P/Invoke
binding's rows keep the binary + JSON + text coverage.

Stage 6 decision: **not attempted in plan 16**. The two below-baseline cells
are the large decodes (0.77x, 0.63x), and the only change that moves them is
unboxed slot storage -- a struct/columnar tree, i.e. a rewrite of `Tree`,
`Encode` and `Decode`, not a one-line probe. Every other cell is at or above
the baseline, so there was no below-baseline cell with a one-change candidate
to A/B; the budget went to the descriptor reader, the decoder's unknown/group/
limit rules and the binary conformance pass instead.

## Cell: Python pure core, every tier (below the extension and upb)

The pure-Python core (`bindings/python/pure/`, issue 41 plan 17) is the
no-extension path, so its baseline is not upb (the C extension in
`google.protobuf`) but the official runtime's own pure-Python mode. Stage 3
provisional numbers, `bindings/python/pbench/compare.py`, best of three
interleaved rounds per cell, 2026-11-21 (Apple silicon; the machine was busy):

| tier | pure-core encode | pure-core decode |
|---|---|---|
| 136 B | 8.8 MB/s | 7.1 MB/s |
| 174 B | 8.8 MB/s | 7.0 MB/s |
| 2469 B | 10.2 MB/s | 6.8 MB/s |
| 74913 B | 10.5 MB/s | 6.8 MB/s |

The same run: `pbwire-py-tree` (the extension's handle path) 953.8 decode /
96092.3 encode MB/s at 74 KB, `pbwire-py-shim` (one downcall, dict in/out)
162.8 / 608.4, upb 806.7 / 514.9. The gap is the interpreter, not the wire
engine: per value the port allocates a Python `int`, `str`/`bytes`, `Raw` or
`dict` slot, which is exactly what upb does not do.

Against the comparable runtime, the same 74913 B payload in a separate process
(`PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3`, best of three
calibrated passes): protobuf's pure-Python parser 6.2 MB/s decode and 15.0 MB/s
serialize-from-parsed. The port is at 6.8 MB/s decode (1.10x) and 10.5 MB/s
`encode(dict)` -- which builds the tree first -- so the port is in the same
class as the official pure-Python runtime, not 100x behind it.

Formal rows: `make python-bench` 2026-11-21
(`results/bench-python-20261121T010913Z.json`, not committed), MB/s per tier:

| tier | core encode(data) | core to_dict(bytes) | core encode(parsed) |
|---|---|---|---|
| 136 B | 8.6 | 7.1 | 13.4 |
| 174 B | 8.8 | 6.7 | 13.0 |
| 2469 B | 10.1 | 6.8 | 18.8 |
| 74913 B | 10.3 | 6.7 | 19.3 |

upb in the same run: 82.4 / 39.3 / 1477.6 encode(data) / to_dict / encode(parsed)
at 74913 B, and the extension's handle path 577.1 / 156.9 / 91672.9. Every core
cell is below its baseline, which is the expected shape for an interpreter-only
codec; the ratio that matters is against the pure-Python runtime above.

### Round 1: inline the decode dispatch -- neutral, reverted

Probe: `python3 bindings/python/benchmarks/bench.py ab <base.py> <cand.py> 3`
(two module paths, interleaved rounds, best-of-pass ns/op per label; the new
A/B mode). Candidate: inline `slot_of`, `wt_incompatible` and `is_packable`
into the decode loop (the three hottest helper calls, ~8% of decode's tottime in
cProfile). Bytes identical.

| cell | base | candidate | ratio |
|---|---|---|---|
| 2469 B to_dict | 360672 ns | 335901 ns | 1.074x |
| 74913 B to_dict | 11691790 ns | 11590926 ns | 1.009x |
| 74913 B encode(data) | 7143315 ns | 7224004 ns | 0.989x |
| 74913 B encode(parsed) | 4109406 ns | 4133122 ns | 0.994x |

The win does not repeat on the published xlarge cell, and the encode rows --
which touch none of the changed code -- moved by ~1%, which is the probe's
noise. Recorded neutral and reverted, per the rule above.

### Round 2: slice-assigned tag writes -- slower, rejected (Task 2, 2026-11-22)

Hypothesis: the six `for c in tag: b[p] = c; p += 1` per-byte loops in the
fused writers (`pure/wire.py`: `w_tag`, `w_tag_var32/64`, `w_tag_fixed32/64`,
`w_bytes`) should lose to one slice assignment (`b[p:p + n_tag] = tag`), a
C-level copy. Candidate: identical `pure/` with only those loops replaced.
Same-host A/B (`bench.py ab`, 3 interleaved rounds, best-of-pass):

| cell | base | candidate | ratio |
|---|---|---|---|
| 2469 B encode(data) | 240186 ns | 254707 ns | 0.943x |
| 74913 B encode(data) | 7155461 ns | 7566128 ns | 0.946x |
| 74913 B encode(parsed) | 3704667 ns | 3872794 ns | 0.957x |
| 74913 B to_dict | 10791998 ns | 10968754 ns | 0.984x |

The candidate loses on every encode cell on both tiers while the untouched
decode row sits at 0.984x (noise). Mechanism: matrix3 tags are 1-2 bytes, so
the slice machinery (slice parsing, temporary objects) costs more than the
1-2 indexed stores it replaces -- the per-byte loop is already near-optimal
at this size. Reverted; the encode walk stays with indexed stores.

Decision (Stage 6, reaffirmed): no further rounds. The below-baseline cells are not a
dispatch problem; 74913 B decode spends its time materializing 3000 messages,
9000 strings/ints/bools and 3000 dicts (cProfile: `read_known` 23%, `to_object`
19%, `to_py`/`materialize` 12%), and the same is true of encode (`from_object`
plus the tree walk). Closing the gap means a different representation -- a
columnar/arena tree or a generated fast path that skips the intermediate
Python objects -- which is a rewrite of the tree and both codecs, not a
one-change A/B round, and it would put the arm at odds with the pbench contract
(a native object in and out). The arm is published as the no-extension path
with its numbers, exactly as the TypeScript and C# cores are.

## Provisional: the Dart pure-core arm (plan 18, 2026-11-21)

Plan 18 landed `bindings/dart/lib/core/`: the third in-language wire
implementation (descriptor reader, `Msg` tree, encode, decode with unknown
retention, groups, packed runs, proto2 required, limits), no `dart:ffi` and no
`dart:io`, so it also compiles to dart2js/dart2wasm where the C library cannot
be reached. The `core` pbench arm (`pbench/c-core.dart`) is the whole user task
-- build the native tree from one plain-Dart entry per field, then `encode()`;
`decode()` then one `get` per field -- exactly like the `tree` arm, so the two
rows differ only in which codec does the work.
`cd bindings/dart/pbench && dart run compare.dart gen/matrix3.desc`,
interleaved rounds, median of five, one session on an M-series laptop (host
load average ~20, other checkouts building), so read the ordering; Stage 5
re-records the formal rows.

| tier | arm | enc ns/op | enc MB/s | dec ns/op | dec MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-dart-core` | 3885 | 35.0 | 7521 | 18.1 |
| 136 B | `pbwire-dart-shim` | 93 | 1462.4 | 133 | 1022.6 |
| 136 B | protobuf-dart | 1365 | 99.6 | 689 | 197.4 |
| 2469 B | `pbwire-dart-core` | 22130 | 111.6 | 37369 | 66.1 |
| 2469 B | `pbwire-dart-shim` | 2295 | 1075.8 | 3599 | 686.0 |
| 2469 B | protobuf-dart | 27196 | 90.8 | 17662 | 139.8 |
| 74913 B | `pbwire-dart-core` | 501564 | 149.4 | 815978 | 91.8 |
| 74913 B | `pbwire-dart-shim` | 68120 | 1099.7 | 100152 | 748.0 |
| 74913 B | protobuf-dart | 908056 | 82.5 | 479490 | 156.2 |

Reading: the in-language codec beats protobuf-dart's generated codec on encode
from 2469 B up (111.6 against 90.8, 149.4 against 82.5) and is 1.8x the
baseline at 74 KB, because the generated encoder walk is per-field virtual and
this one is a flat slot loop. It loses the small tiers on both ops (0.35x /
0.09x at 136 B) where 3000-message-free work is dominated by per-call overhead:
one `Msg` with a 22-slot list, one `fromObject` walk, one workspace buffer copy
per encode, and -- on decode -- one `Msg` per nested message plus one growable
list per repeated field. Decode is below the baseline at every tier (0.09x to
0.59x): the tree materializes every element, which the generated reader does
too, but the dynamic slot indirection (`slotOf` + `fields[s]` + `Object` casts)
costs more than a compiled field write.

Conformance scope: binary only (the plan's file list has no JSON or text
printer), so `make conformance-dart-core` passes the binary suite with the two
expected tag-truncation failures and skips the JSON/text half: 1412 successes,
1403 skipped, 2 expected failures, 0 unexpected -- the same set as the C core,
the TypeScript, C# and Python ports. The FFI testee `make conformance-dart`
keeps covering binary + JSON + text (2750 + 439).

Formal rows (`make dart-bench`, `results/bench-dart-20261121T013521Z.json`,
MB/s at 136 / 174 / 2469 / 74913 B): `pbwire-dart-core` encode 66.9 / 65.4 /
312.3 / 466.7 and decode 70.5 / 79.4 / 172.8 / 209.9; protobuf-dart the same
run 194.4 / 174.5 / 118.5 / 101.3 and 214.7 / 189.3 / 143.3 / 159.0. The
published rows are prebuilt-tree ops, so the below-baseline cells are exactly
the two small tiers (0.34x / 0.33x and 0.37x / 0.42x); from 2469 B the core is
above the baseline on both ops (3.11x / 1.19x) and 4.82x / 1.32x at 74 KB.

Stage 6 decision: **recorded, not chased**. Both below-baseline cells are the
136 B and 174 B tiers, and their cost is fixed per message, not per byte: one
`Msg` with a 22-slot `List` and one growable `List` per repeated field per
decode, one `fromObject` walk plus one workspace copy per encode, and a dynamic
`slotOf` + `fields[s]` + cast per field where the generated reader writes a
compiled field. A one-change-per-round probe cannot move that: the candidates
(hoist the field table into a local, cache `fields.length`, drop the workspace
copy) are each worth a few percent of a 2 µs op, and the honest fix is a
columnar/lazy tree or code generated for the concrete message -- a rewrite of
the tree and both codecs, and what the FFI runtime already is. The arm is
published with its numbers as the portable (no native library, dart2js/
dart2wasm) path, exactly as the TypeScript, C# and Python cores are.

## Provisional: the Julia pure-core arm (plan 19, 2026-11-21)

`bindings/julia/runtime/src/core/` is the pure-Julia core: descriptor reader,
`Message` tree, wire primitives, encoder and decoder (unknown retention,
groups, packed runs, oneof, presence, limits), stdlib only -- no `ccall` and no
native library, so it is the arm for hosts that cannot build the C core. The
`core` pbench arm (`pbench/core_arm.jl`) is the same user task as the other
bindings' core arms: a plain `Dict` in, `encode_dict` out; wire bytes in,
`decode_dict` (fully materialized) out -- so the timer covers the tree
conversion a user of this path pays, not a prebuilt tree.

`cd bindings/julia/pbench && julia --project=../runtime compare.jl`,
interleaved rounds, median of five, one session on an M-series laptop (host
load average ~20, other checkouts building), so read the ordering; Stage 5
re-records the formal rows.

| tier | arm | enc ns/op | enc MB/s | dec ns/op | dec MB/s |
|---|---|---|---|---|---|
| 136 B | `pbwire-julia-core` | 2787 | 48.8 | 788 | 172.6 |
| 136 B | `pbwire-julia-shim` | 102 | 1334.6 | 119 | 1145.2 |
| 136 B | protobuf-jl | 348 | 390.6 | 106 | 1279.5 |
| 2469 B | `pbwire-julia-core` | 84114 | 29.4 | 95375 | 25.9 |
| 2469 B | `pbwire-julia-shim` | 2396 | 1030.5 | 3606 | 684.7 |
| 2469 B | protobuf-jl | 22453 | 110.0 | 4048 | 610.0 |
| 74913 B | `pbwire-julia-core` | 2493871 | 30.0 | 2980705 | 25.1 |
| 74913 B | `pbwire-julia-shim` | 67040 | 1117.4 | 100088 | 748.5 |
| 74913 B | protobuf-jl | 6296130 | 11.9 | 102895 | 728.1 |

Reading: the pure core's encode crosses ProtoBuf.jl at 74 KB (30.0 against
11.9 MB/s): the generated Julia encoder re-derives sizes per field, while this
one writes fused tag+value pairs into one reusable buffer. It loses the three
smaller encode tiers (0.12-0.27x) and every decode tier (0.03-0.13x): the
`Dict` <-> tree conversion is real work per field (`from_dict`/`to_dict` walk a
`Vector{Any}` slot table and the decoded strings stay `Raw` slices until
materialized), and ProtoBuf.jl's generated decode writes straight into typed
struct fields. The interpreter loop is published as the no-native path, never
as the fast one: the `shim` row (one `ccall` per op) is 2.4-4.8x the core
encode and 2.2-4.8x the core decode.

Conformance scope: binary only (the plan's file list has no JSON or text
printer), so `make conformance-julia-core` passes the binary suite with the two
expected tag-truncation failures and skips the JSON/text half: 1412 successes,
1403 skipped, 2 expected failures, 0 unexpected -- the same set as the C core,
the TypeScript, C#, Python and Dart ports. The `ccall` testee
`make conformance-julia` keeps covering binary + JSON + text.

Formal rows (`make julia-bench`, `results/bench-julia-20261121T020057Z.json`,
MB/s at 136 / 174 / 2469 / 74913 B): `pbwire-julia-core` encode 52.7 / 25.8 /
21.7 / 22.1 and decode 164.6 / 46.8 / 23.5 / 22.5; protobuf-jl the same run
383.1 / 322.7 / 130.3 / 12.1 and 1076.1 / 763.7 / 618.6 / 695.9. The published
rows are prebuilt-object ops (`encode_dict` on a Dict held before the timer,
`decode_dict` materializing the whole dict), so the below-baseline cells are the
three smaller encode tiers (0.08-0.17x) and every decode tier (0.03-0.15x); the
one above-baseline cell is the 74 KB encode (1.83x), where ProtoBuf.jl's
per-field size pre-pass dominates and the flat slot loop does not.

Stage 6 decision: **recorded, not chased**. The only above-baseline cell (74 KB
encode, 1.83x) needs no work; every below-baseline cell is a decode or a small
encode, and their cost is the `Dict` <-> tree conversion, not the wire walk: a
`Vector{Any}` slot table written and read per field, one `Any` vector per
repeated field, one `Raw` slice per decoded string, and `from_dict`/`to_dict`
name lookups where ProtoBuf.jl writes and reads typed struct fields. A
one-change-per-round probe cannot move that -- the candidates (hoist the field
table into a local in `encode_into`/`decode_into`, cache `def.fields` length,
drop the result copy) are each worth a few percent of a 2.6 us encode or a
826 ns decode, which is inside this host's run-to-run spread (the same session
moved the tree arm's xlarge encode from 1653 to 1718 MB/s). The honest fix is a
columnar/lazy tree or code generated for the concrete message -- a rewrite of
the tree and both codecs, i.e. the `ccall` runtime again. The arm is published
with its numbers as the no-native path, exactly as the TypeScript, C#, Python
and Dart cores are.

## Provisional: the Rust pure-core arm (plan 20, 2026-11-21, issue 41)

`bindings/rust/src/core/` is a pure-Rust port of the C core behind the `core`
cargo feature (off by default: the crate keeps linking the C core, `make
rust-test` keeps proving the FFI path). It is the `core` arm of the three-arm
contract and it competes with prost directly, since prost is a pure-Rust codec
too. `make rust-bench`, one session, M-series arm64 laptop; `core` is a prebuilt
`Message` tree through one `encode_to` into a reused buffer and one one-shot
`decode`, exactly the shape of the `pbwire-rust-tree` row, so the two rows
differ only in who walks the wire:

| tier | arm | enc MB/s | dec MB/s |
|---|---|---|---|
| 136 B | `pbwire-rust-core` | 1119.2 | 433.3 |
| 136 B | prost | 1654.1 | 916.3 |
| 136 B | protobuf-rust | 1325.8 | 769.0 |
| 2469 B | `pbwire-rust-core` | 889.1 | 236.2 |
| 2469 B | prost | 1259.0 | 335.8 |
| 2469 B | protobuf-rust | 854.6 | 378.6 |
| 74913 B | `pbwire-rust-core` | 895.2 | 233.3 |
| 74913 B | prost | 1310.9 | 357.2 |
| 74913 B | protobuf-rust | 864.5 | 379.9 |

Reading: the core is above protobuf-rust on the two large encodes (1.04x) and
below prost on every encode (0.68-0.69x); every decode is below both baselines
(0.47-0.70x of prost). The shape is the same one the other host-language cores
report: prost's generated writer and reader are straight-line code over typed
struct fields with no value tree in between, while this arm's tree is a slot
vector of a `Value` enum, one `Box<Message>` per submessage and one byte vector
per string. Those are allocations on the decode path the reference does not
make -- the payload's 3000 `inners` and 3000 `tags` at 74 KB -- which is the
decode gap, not the wire walk (the encode side does no allocation at all).

Conformance: `make conformance-rust` (the new `bindings/rust/conformance/main.rs`
testee, built with `--no-default-features --features core`, so no C code is
linked) passes the binary suite with the same failure set as the C core: 1412
successes, 1403 skipped, 2 expected failures, 0 unexpected. The skipped half is
the JSON/text mapping, out of this core's (binary) scope.

Formal rows (`make rust-bench`, `results/bench-rust-20261121T022430Z.json`, MB/s
at 136 / 174 / 2469 / 74913 B): `pbwire-rust-core` encode 1008.3 / 978.2 / 869.0
/ 923.2 and decode 413.0 / 348.4 / 237.1 / 238.4, against prost's 1563.4 /
1516.8 / 1242.7 / 1348.3 and 884.6 / 612.7 / 326.2 / 363.3 (0.64-0.70x encode,
0.47-0.73x decode) and `rust-protobuf`'s 1282.8 / 1317.3 / 831.4 / 886.2 and
731.4 / 571.2 / 368.7 / 400.7: above it on the two large encodes (1.04-1.05x),
below it on every decode (0.56-0.64x). The shipped `pbwire-rust-tree` row is
1.66-1.88x the core on encode and 2.50-3.14x on decode, which is why the static
link stays the default.

Stage 6 decision: **recorded, not chased**. Every below-baseline cell is a
decode (0.47-0.73x of prost) or a small encode; the encode path already
allocates nothing (one reusable buffer, fused tag+value writes) and lands within
4% of `rust-protobuf` at 74 KB, so the encode cells are prost's straight-line
generated writer, not a missing reserve. The decode cost is the boxed tree the
C core's shape forces on a schema-driven codec: a `Vec<Option<Value>>` slot
array per message (zeroed on every submessage), a `Box<Message>` per submessage
and one owned byte vector per string -- about 12k allocations for the 74 KB
payload's 3000 `inners`, 3000 `tags` and 3000 `nums`. The candidates that fit
"one change per round" (reserve the packed varint run up front, shrink `Value`
by boxing its `Vec` payloads, `Box<[u8]>` strings) each move a few percent of a
314 us decode, i.e. inside this host's spread, and the change that would close
the gap is a columnar or code-generated tree -- the rewrite the TypeScript, C#,
Python, Dart and Julia cores also declined. The arm is published with its
numbers as the no-C path.

## Provisional: the Swift pure-core arm (plan 21, 2026-11-21, issue 41)

`bindings/swift/Sources/PbwireCore/` is a pure-Swift port of the C core, shipped
as the `PbwireCore` product beside the FFI `Pbwire` product. It exists because
the FFI package cannot be a normal SwiftPM dependency: its products carry
`LinkerSetting.unsafeFlags(["../../build/libpbwire.a"])`, which SwiftPM rejects
in a non-root package. `PbwireCore` has no C module, no archive and no unsafe
flags, so `swift build --product pbwire-conformance` links only Swift.

`swift run -c release Bench compare`, one session, M-series arm64 laptop; the
core arm is a prebuilt `PbwireCore` `Message` tree through one `encode()` / one
`decode()`, exactly the shape of the `pbwire-swift-tree` row, so the rows differ
only in who walks the wire. MB/s at 136 / 174 / 2469 / 74913 B, before the stage
6 rounds: `pbwire-swift-core` encode 163.3 / 94.3 / 82.8 / 185.3, decode 75.0 /
47.8 / 22.2 / 51.2, against the shipped handle's 1023.0 / 742.9 / 694.5 /
1518.8 and 836.1 / 497.4 / 304.6 / 592.1.

Conformance: `make conformance-swift` (the new testee in
`bindings/swift/conformance/`) passes the binary suite with the same failure set
as the C core — 1412 successes, 1403 skipped, 2 expected, 0 unexpected.

First reading before any attempt: the Rust core (the same port shape, a
`Vec<Option<Value>>` tree) does 923 MB/s encode and 238 MB/s decode at 74 KB, so
a 5x gap at equal algorithm says Swift-side overhead outside the wire walk, not
a worse codec. Candidates, one per round: a class `MsgDef` (every
`schema.messages[i]` read copies a struct with two dictionaries and three
arrays), an inline `Tag` (every tag write currently retains a `[UInt8]`), and
per-message `Field` copies.

Stage 6 round 1 (kept): type-level overhead, not the wire walk. `sample` on the
`Bench` process showed the arm spending most of its time in
`swift_beginAccess` (the `Reader` was a class, so every `pos` read/write cost a
runtime exclusivity check -- per byte), in `outlined init with copy of Tag`
(every tag was an `[UInt8]`, i.e. ARC traffic per field), and in `MsgDef` struct
copies (`schema.messages[i]` copied two dictionaries and three arrays per
message and per field). One change, three mechanical parts: `Reader` is a value
type passed `inout`, `Tag` holds its five bytes inline, `MsgDef` is a class.
Wire bytes unchanged (the codec gate and the official suite re-pass); xlarge
encode 185.3 -> 239.6 MB/s and decode 51.2 -> 60.2 MB/s, 136 B encode 163.3 ->
232.8 and decode 75.0 -> 93.0. Keeping it.

Stage 6 decision after round 1: **recorded, not chased further**. Every cell is
still below swift-protobuf (0.54-0.90x encode, 0.28-0.43x decode), and the
remaining cost is the tree itself, not the codec: one class `Message` + one
`[Value?]` slot array per submessage, one `[UInt8]` per string, and a
`Value`/`Field` copy per field -- 3000 of each in the 74 KB payload. Two more
rounds were measured as candidates and are inside the host's spread or start
changing the published object shape: making `Field` a class (one retain instead
of two string retains per field, single-digit percent) and hoisting the
`values` array out of ARC with an unsafe buffer (the message tree would lose its
value semantics at the API edge). The honest fix is the same one the Rust,
TypeScript, C#, Python, Dart and Julia cores recorded: a columnar or
code-generated tree, i.e. a rewrite of the tree and both codecs. The arm is
published with its numbers as the no-C, no-`unsafeFlags` path -- the product
exists for reach (a normal SwiftPM dependency), and `make conformance-swift`
proves the wire format on it.

## Provisional: the Zig pure-core arm (plan 22, 2026-11-21, issue 41)

`bindings/zig/core/` is a pure-Zig port of the C core, selected by
`-Dcore=true` in `bindings/zig/build.zig` (no C source, no translate-c, no
`libpbwire.a`). The default build still compiles and links the C core; the
option exists for a build that cannot compile `src/*.c`. The pbench arm is
`pbench/core_arm.zig`: a prebuilt core `Message` tree through one `encodeTo`
into a reused buffer and one one-shot `decode`, the same shape as
`pbwire-zig-tree`, so the rows differ only in who walks the wire.

`zig build -Doptimize=ReleaseFast bench`, one session, M-series arm64 laptop,
MB/s at 136 / 174 / 2469 / 74913 B: `pbwire-zig-core` encode 574.4 / 565.9 /
685.7 / 729.9 and decode 780.6 / 649.4 / 326.8 / 334.7, against the shipped
`pbwire-zig-tree` 2017.8 / 1804.9 / 1687.4 / 1641.3 and 1081.4 / 907.9 / 665.8 /
743.7 and the shim 1849.5 / 1515.0 / 1118.5 / 1200.7 and 1211.9 / 1065.8 /
658.9 / 721.2. The tree row is the C core's own encoder; the pure core pays for
its value tree and per-field allocations on decode, and for tag/length writes
without the C encoder's reserve pass on encode.

Formal rows (`make zig-bench`,
`results/bench-zig-20261121T032028Z.json`, MB/s at 136 / 174 / 2469 / 74913 B):
`pbwire-zig-core` encode 549.1 / 563.1 / 677.7 / 731.9 and decode 771.4 /
649.0 / 343.6 / 356.6; `pbwire-zig-tree` 1986.1 / 1861.7 / 1658.6 / 1604.0 and
1043.6 / 910.6 / 651.4 / 724.2; the `shim` 1735.6 / 1654.5 / 1131.2 / 1197.1
and 1224.9 / 1037.7 / 673.1 / 745.0; zig-protobuf 1279.4 / 1040.3 / 834.5 /
825.6 and 847.2 / 730.6 / 593.6 / 521.8. That is 0.43-0.89x encode and
0.58-0.91x decode of the generated baseline, so the core stays the no-C option,
never the shipped fast path. Both harnesses now allocate through the release
allocator on both sides; the earlier zig-protobuf decode figures (~25 MB/s)
were measuring a debug allocator, not the baseline.

Stage 6 decision: **recorded, not chased.** The below-baseline cells are the
whole core arm, and the honest fix is a code-generated or columnar tree, not a
local edit: the decode row allocates one arena, a value slice per message and
one byte slice per string, while the encode row appends tag/length bytes through
the generic array writer. The same conclusion as the Rust and Swift cores; the
arm is published with its numbers as the no-C build.

Conformance: `make conformance-zig-core` (the new
`bindings/zig/conformance/testee_core.zig`, built with `-Dcore=true`) passes the
binary suite with the same failure set as the C core: 1412 successes, 1403
skipped, 2 expected, 0 unexpected. The skipped half is JSON/text, out of this
core's binary scope.

## Cell: Lua shim decode, all tiers (plan 2026-11-22 Task 1, pilot)

`make lua-bench` (`results/bench-lua-20261122T160929Z.json`, Apple M4 Pro,
Lua 5.5.1): `pbwire-lua-shim` decode 147.0 / 142.8 / 131.6 / 134.6 MB/s
(1.43x-1.61x vs lua-protobuf) — already the winning native arm, kept by the
Task 1 selection. One optimization round, same-host A/B, interleaved
base/candidate processes (5 rounds each, median reported, small + xlarge
tiers): merge the count-then-fill loops in `pbwlua_push_message`
(`bindings/lua/runtime/pbwire.c`) into a single pass — presize the table
with the field count and do one `pbw_message_get_field` per field instead of
two.

Result: xlarge 540209 -> 536867 ns/op (+0.6%), small 887.3 -> 872.4 ns/op
(+1.7%) — inside the noise floor, and the mechanism does not explain more:
`pbw_message_get_field` is O(1) (fresh check + bounds + presence bit), so the
removed pass is ~66k lookups x ~5 ns = ~0.3 ms against a ~540 ms xlarge
decode (0.06%). Reverted: the exact-size hash (a sparse message keeps a small
hash) is worth more than a sub-percent decode move.

The encode side was analyzed but not attempted: per-field cost is one
`rawgeti` + `rawget` per known field (~11 ns measured in pure Lua for the
pair) plus one irreducible `lua_next` unknown-key scan per message (~27 ns
per key measured — the scan IS the contract: unknown keys must fail, and any
correct implementation visits every key at least once). The single-pass
alternative (dispatch every key by interned-pointer probe, stash values in
stack slots, emit in descriptor order) saves ~5 ns/field at best against a
far more intricate hot path, so it stays unbuilt. No `runtime/` change from
the perf round; the selection (shim already shipped) stands.

## Cell: Java tree encode/decode at xlarge (plan 2026-11-22 Task 3, JVM group)

Reproduce (`make java-bench`, same host that ran the Task 1/2 benches above;
`results/bench-java-20261122T163333Z.json`, pre-change): xlarge 74913 B,
same-run ratios vs `protobuf-java` — `tree` encode 0.279 / decode 0.845,
`core` 0.693 / 0.805. Small tier: `tree` encode 0.238 / decode 0.910.
Matches the brief's red cells (0.28x/0.87x, core 0.63x/0.81x) within the
documented ±10% JVM run variation.

Profile (throwaway `/tmp` probes against the bench classes, then JFR
`ExecutionSample` on the xlarge loops — no repo harness touched): the tree
encode op is 83% `fillGeneric`, not `encodeInto`. Split at xlarge: DMsg
build ~248 us, encode of the held tree ~66 us, while the prebuilt
`proxy` encode already beats the baseline 1.11x. The `tree` arm builds
fresh per iteration by contract (the Go-parity close: "the gap the old
prebuilt tree row was hiding" is priced in on purpose program-wide — Node
33.7x, C# 21x — and the baseline's prebuilt encode is the disclosed
steady-state side), so the red cell is the dynamic build price: ~12000
name-keyed `set`/`add`/`newMessage` calls per xlarge iteration (HashMap +
second fnum->slot lookup + re-dispatch each), ~6000 `Integer` boxes, 3001
`DMsg` allocations. JFR on the encode op: `DMsg.add` on 65% of stacks,
`newMessage` 32%, `slotByName` 15%; `growLong`/`growObj` near zero
(growth is amortized, not a target — the earlier larger-minimum attempt
stayed rejected). Decode split: lazy `decode` ~90 us, `consumeTree`
readback ~187 us (materializes all 3001 inner Slices + two `getList`
copies); JFR: `decodeInto` first, then `getList`/`materialize`/`readKnown`,
`wtIncompatible` on ~29% of stacks, `validateInto` meaningful (full
re-validation walk per inner, required: decode must fail on invalid bytes
even when the field is never read).

### Kept (1): name-keyed DMsg writes resolve once

`set/add/newMessage(String)` resolved name->number->slot and re-ran the
numeric path's checks per call; `newMessage(String)` paid three
resolutions (map + slot + the inner add/set re-dispatch). The string
entries now resolve name->slot once and run the slot-direct bodies
(`setSlot`/`addSlot`, both funnelled from the int entries so the two
spellings share one body; `newMessage` funnels to the existing
`newMessageInSlot`). Same exceptions for the same inputs (`slot()` still
validates the numeric path; `fields[s].number == fnum` whenever the slot
is valid, so the messages are unchanged).

A/B: alternating separate JVMs, `TIER=xlarge` Bench, base vs candidate
class dirs, `-Xms1g -Xmx1g -XX:+UseSerialGC`, tree/core/proxy rows every
round (4 rounds). xlarge tree encode ns/op, base -> cand: 285728 -> 250382
(-12.4%), 283188 -> 250597 (-11.5%), 287044 -> 250147 (-12.9%),
284520 -> 252369 (-11.3%). Controls flat every round (core encode +0.7%
mean, proxy encode -0.2%, core/proxy decode mixed ±3%): the win is the
change, on the only path it touches. Tree decode mixed ±1% (mutation-only
change, as predicted). Kotlin/Scala/Clojure inherit it: all three DSLs
build through these string entries (`fillPbwire`/`message(name)`/
persistent-map assoc over `set`/`newMessage`).

### Kept (2): the tag wire-type gate is inlined at both dispatch loops

`wtIncompatible` (one call + one field re-fetch per tag occurrence,
~6000/iter at xlarge plus the same again inside `validateInto` and the
materializing re-decodes) is now one branch shape at the `decodeInto` and
`validateInto` loops, same accept set (groups only open; packed LEN
whatever the declared form; packed field still takes the unpacked singular
form; else `wt != f.wt`; unknown numbers still retain). The helper is
deleted; both loops fetch the field once and pass it to
`readKnown`/`validateKnown`.

A/B (same rig, base = change 1): xlarge tree decode base -> cand:
201345 -> 198275 (-1.5%), 202305 -> 195854 (-3.2%), 204870 -> 198615
(-3.1%), 201253 -> 198192 (-1.5%). Corroboration from the pure changed
code: `proxy` decode (decodeInto only, no readback) -4.8%/-6.1%/-7.8%/-6.0%
all four rounds; untouched paths mixed (tree/core/proxy encode means
+0.9%/+0.0%/+2.0% with one op-level calibration outlier each way in round
1 — per-op calibration noise, opposite directions in the same JVM, not box
drift). Tree decode moves less than proxy decode because the readback
(materialization + `getList` copies) is untouched: -2.3% mean on the
published op.

### Rejected / not attempted (with reasons)

- Faster name->slot table (custom open-addressed map with reference-equality
  fast path for interned literals, replacing the `HashMap.get` inside
  `slotByName`): analyzed, not built. `HashMap.get` already reference-checks
  first, so the saving is ~2 ns x ~12000 calls ≈ 8% of the op at best,
  against a second per-type structure with its own lifecycle to review (the
  review focus already flags per-type caches) — a second encode change for a
  single-digit move on a cell whose floor is structural (below).
- Lazy validation skip (record slices without `validateInto`, validate on
  first read): rejected without building. Decode must fail on invalid bytes
  even for unread fields (conformance `parse_error` + the proxy
  decode-and-drop op), so the double traversal is the laziness price, not a
  bug. Same reason the `Slice`-per-element record stays: the ref must point
  at something, and the object is minimal.
- Splitting `readKnown` for inlining: no mechanism. The 7-arg call stands;
  the logic has to live somewhere and both dispatch sites are already flat.
- `newMessage`/`DMsg` allocation or boxing removal on the build: intrinsic
  to the arm (fresh tree per iteration through the `Object` API). Only
  per-message generated writers remove it — the product decision the Java
  136 B cell already recorded as rejected.

### Task 15 disposition (JVM slice): floors documented, two keeps banked

After both keeps, the formal re-run
(`results/bench-java-20261122T165732Z.json`, same host, higher load than
the pre-change run — baseline/proxy uniformly ~+1-9%, so only same-run
ratios read): xlarge `tree` encode 0.284 / decode 0.826, `core` 0.699 /
0.791; the A/B deltas (-12% encode, -2%/-6% decode) sit inside this run's
load noise, which is why the keeps are claimed from the interleaved A/Bs,
not the formal table. The cells stay red and are now floors, each with its
mechanism: tree encode is the dynamic build (boxing + per-message
allocation + per-field dispatch vs generated typed builders — ~227 us of
the 297 us op; the codec itself, proxy-vs-baseline, is 1.09x); tree decode
is the lazy double-touch (validate + record, then materialize + copy vs
one eager generated pass); core encode/decode is the `PbCodec`/`FieldMeta`
per-field table (flat in every A/B control, confirming the gap lives there;
the formal run's core-encode 0.63x -> 0.70x is run noise on a JIT-sensitive
lambda path, not a change effect — nothing on that path was touched;
closing it needs per-message generated writers, already rejected). The
Kotlin 136 B encode cell (0.71x) is the fixed per-op cost
(ThreadLocal workspace + buffer + copyOut vs a memoized baseline write at
136 B) on the riders' pre-parity prebuilt basis — structural at that size,
not attempted. Transfer check (full-tier A/B under formal conditions:
`bench.sh` straight, all four tiers, default flags, base/cand alternating,
2 rounds): tree encode base -> cand 308045 -> 286720 (-6.9%) and 302670 ->
288859 (-4.5%); tree decode +0.4%/+1.7%; untouched core rows mixed ±7%.
The encode keep transfers at about half strength (tier-order/G1 state halves
a dispatch-shape win but never flips it: 8/8 JVMs across three rigs); the
decode keep sits inside full-tier noise (the isolated -2%/-6% stands on
mechanism + proxy corroboration, and the full-tier runs contradict nothing —
same sign or nil, never a consistent regression). Both keeps stand as filed.
No further untried one-change mechanism on any JVM native
cell; the JVM slice of Task 15 is closed.

Conformance gap closed beside the perf work: `make conformance-kotlin`
is new (`bindings/kotlin/conformance/Testee.kt`, same schema-driven shape
as the Scala/Clojure testees over the shared runtime; `KOTLIN_CP` =
the fat jar). Result: 443 successes, 0 unexpected — the same count as
java/scala/clojure on this host.

## Cell: Ruby shim encode/decode vs the gem (plan 2026-11-22 Task 4, C-extension group)

Formal `results/bench-ruby-20261122T015616Z.json`: `pbwire-ruby-shim` 0.19x/0.23x
at xlarge (0.18x-0.58x across tiers), `tree` 0.03x/0.08x. The encode walk
(`object_fill`) and the decode walk (`object_from_tree`) each paid one
`pbw_descriptor_field_at` index fetch per field per object on top of the
work itself (ivar get/set, scalar convert, tree store), while the per-field
IDs, the ctx arenas and the preallocated decode arrays were already in
place. One round, same-host A/B (`pbench/compare.rb`, 3 runs each side,
median-reported ns/op): cache the `pbw_field *` in `field_t` at setup
(`f->df`, `bindings/ruby/ext/pbwire_ext/pbwire_ext.c`) and use it at all
five hot sites (fill, from_tree, set, get, required-check).

A/B (base -> cand, ns/op, lower wins): small enc 823.3 -> 803.3 (-2.4%),
small dec 1047.3 -> 1039.0 (-0.8%), medium enc 976.8 -> 966.6 (-1.0%),
medium dec 1243.8 -> 1238.1 (-0.5%), large enc 11637.8 -> 11611.8 (-0.2%),
large dec 18065 -> 17904 (-0.9%), xlarge enc 312384 -> 308546 (-1.2%),
xlarge dec 501830 -> 499401 (-0.5%). Every cell moves the same direction
across all three rounds (small-encode ranges do not overlap); the
mechanism explains it (one descriptor indirection removed per field per
op, nothing else touched). Kept: `make ruby-test` 41/41,
`pbench/gate.rb` PASS, `make conformance-ruby` 443/0. The keeps are
claimed from the interleaved A/Bs, not a formal re-run (same convention
as the Task 3 JVM keeps): the cells stay red and are now floors — the
remaining gap is the model materialization itself (ivar walk + per-scalar
convert + tree store vs the gem's C-backed generated objects), which no
index-fetch removal closes.

## Cell: Node shim encode vs jspb (plan 2026-11-22 Task 4, C-extension group)

Formal `results/bench-node-20261121T181411Z.json`: `pbwire-node-shim`
1.02x/0.64x vs jspb at xlarge (0.12x/0.13x vs protobufjs). The shim encode
paid two N-API crossings per op: `encodeObject` returned the scratch
length as a double, then JS `_copy` ran `Buffer.allocUnsafe` +
`copyEncoded` (second downcall + memcpy). One round, same-host A/B
(`pbench/compare.js`, 3 runs each side): return the bytes directly with
`napi_create_buffer_copy` (one downcall, one copy), keeping the
stale-scratch state machine (`enc_valid` consumed, so a stray
`copyEncoded` still reports "without a preceding encode" as the test
pins).

A/B (base -> cand, ns/op): small enc 2822 -> 2937 (+4.1%), medium enc
3383 -> 3613 (+6.8%), large enc 46411 -> 46391 (nil), xlarge enc 1309020
-> 1280420 (-2.2%); decode untouched (nil both tiers, as expected). The
mechanism explains both directions: for small buffers the old path's
`Buffer.allocUnsafe` comes from the JS slab pool while
`napi_create_buffer_copy` mallocs, so the saved crossing (~60-70 ns) does
not cover the allocator gap; at xlarge both paths malloc and the saved
crossing wins. Rejected: it trades a small-tier regression (the sizes
users encode most) for an xlarge-only win. Reverted to base
(`addon.c` + `lib/pbwire.js` unchanged); `make node-test` 70/0,
`pbench/gate.js` PASS on the kept tree. The floor stands as the pbench
README documents it: one N-API property read/write per field (~60-70 ns)
vs a few ns in jspb/protobufjs's own heap.

## Cell: PHP shim encode/decode vs the official C extension (plan 2026-11-22 Task 4, C-extension group)

Formal `results/bench-php-20261122T022541Z.json`: `pbwire-php-shim`
0.17x/0.37x at xlarge (0.15x-0.21x / 0.37x-0.41x across tiers),
`tree` 0.07x/0.09x. Spot re-time this host (`compare.php` with the
rebuilt ext): shim xlarge 356187.7 ns/op (210.3 MB/s) encode, 366150.2
(204.6) decode — same shape as formal. The proxy row in the same formal
run is 1612.8/713.2 (19.0x/13.4x the shim at xlarge): the gap between
re-encoding a held tree and walking a PHP array field by field is the
floor, not a bug. Two candidates analyzed, neither built:

- Interned field-name keys per Type (skip the per-op name hash in
  `zend_hash_str_find`/`add_assoc_zval_ex`): reach stops at the top
  level. The nested walk (`array_encode_message` /
  `array_decode_message` recurse through descriptors, not Type objects),
  where thousands of probes happen at xlarge, has no Type to hang a
  cache on — a per-descriptor cache from the binding is out (core owns
  the descriptor, and the review focus already flags per-type caches).
  Saving one hash per top-level field (~40 at xlarge) against a new
  lifecycle in `type_obj` (interned-string array + free handler + GC
  visibility) prices out at single-digit percent of one op at best.
- Array-order iteration instead of schema-order probing (foreach the
  input keys, resolve each with `pbw_descriptor_find_field_name`):
  xlarge messages are dense (probe count ≈ key count), and sparse tiers
  would trade N cheap probes for N descriptor hash lookups — no
  crossover mechanism, same steady state either way.

No `ext/` change from the perf round. The `array_scalar_value` path was
re-read in the same pass (direct zval reads, ctx arenas, no copies) and
holds nothing further single-change-sized.

## Cell: R native arms vs RProtoBuf (plan 2026-11-22 Task 4, C-extension group)

No red cell: formal `results/bench-r-20261122T020549Z.json` has `shim`
22x-60x and `tree` 8x-38x above RProtoBuf on every tier and op, and the
pbench verdict already records "nothing to optimize here". The round
was a verification re-time plus assessment, not a change:
`pbench/compare.R` on this host confirms the shape (shim 25x-60x,
tree 8x-45x; one xlarge-encode round had tree ahead of shim, 572265 vs
646484 ns/op — the reverse of formal — which is host-load movement on a
fully green board, not a finding). Assessed and not attempted: decode
materialization already preallocates every vector by count
(`allocVector(VECSXP/STRSXP/...)` sized from `pbw_value_count`,
`runtime/src/pbwire_r.c`); the remaining cost is the listed-API shape
itself (named-list walk + per-scalar convert vs RProtoBuf's R-level
per-field work on C++). No `runtime/` change; `make r-test` and
`make conformance-r` (443/0) stand as the proof.

## Cell: C# shim decode at xlarge (plan 2026-11-22 Task 5, .NET)

Formal `results/bench-csharp-20261122T181429Z.json` (this host): `shim`
decode 0.71x at 74913 B (357.9 vs 504.9 MB/s), 0.72x at 2469 B; encode
above baseline on all four tiers (1.12x-2.54x). Same-host A/B, interleaved
`pbench/compare` (median of 5, in-process `protobuf-csharp` as drift
control): presize the four repeated lists in generated `ReadImage`
(`if (Nums.Capacity < nNums) Nums.Capacity = nNums;`, never shrunk so a
reused model keeps its larger buffer) before the fill loops.

Result: xlarge 0.698x -> 0.735x (220898 -> 208795 ns/op against a baseline
that moved 154305 -> 153435, +0.6% drift), large 0.757x -> 0.803x; small /
medium inside their usual spread (3x+ round spread, 0-1 elements, nothing
to presize). Mechanism: ~48 growth reallocations per xlarge decode removed
(4 lists x ~12 doublings to 3000). Kept: one line per repeated field in
`codegen/internal/gen/csharp.go`, gate byte-identical both ways,
`make csharp-test` green.

No further round on this cell: the remainder is the managed
materialisation the pbench README already splits (native decode faster
than the reference parser's whole run; 3000 ints + 3000 strings + 3000
sub-messages + 3000 bools into objects costs more), which no boundary
tuning removes short of dropping the image copy -- a mechanism change,
not a round.

## Cell: C# core honest re-baseline (plan 2026-11-22 Task 5, .NET)

Not a tuning round: the published `core` row (1.34x / 0.62x at xlarge)
timed a `Msg` tree built once before the timer -- a proxy row in a core
label. Aligned to the arm contract (plain object in, convert + codec per
call, same prebuilt native input every arm encodes from), the row is
0.53x / 0.25x at xlarge, 0.46x / 0.35x at 2469 B, 0.67x-0.71x encode at
136/174 B. No code change was attempted to recover the old numbers: the
conversion (`Dictionary<string, object?>` with string keys per message,
boxed slots) is the task, the same class as Python's accepted
`encode(data)` 0.69x. Published as-is; the core stays the no-toolchain
arm, never the headline.

## Cell: Dart tree encode at large (plan 2026-11-22 Task 6, static-link group)

Worst red cell on a non-shipped arm: `tree` encode 0.39x at 2469 B vs
protobuf-dart (0.46x/0.49x/0.50x on the other tiers; the shipped `shim`
row is 6.6x-11.0x everywhere). Micro-split on the same host
(`pbench/micro_split.dart`, since removed; best of 5): build-only 67540
ns/op, encode-only 2000 ns/op, decode+read 7265 ns/op -- the build is
97% of the row, ~109 ns per set at 100 repeats (~620 sets).

Per-set split (`pbench/micro_set.dart`): `Type.field(name)` resolve
9 ns/op, `setInt` on a cached `Field` 36 ns/op (one `valueNewInt` FFI +
C malloc plus one `messageSet` FFI). Same-host A/B of by-name build vs
a pre-resolved-`Field` build (`pbench/micro_ab.dart`): 69990/69624
ns/op vs 66493/64851 ns/op, i.e. +5.0%/+6.9% -- the whole Dart-side
share, and it redefines the timed task (the arm prices the shipped
by-name per-field API, the same task `protobuf-dart`'s generated
setters do without a lookup).

Rejected: the remaining ~93% is two FFI crossings plus one C malloc per
set, which is the priced boundary, not a tuning gap -- the same class
as Go's `dync` (0.05x-0.13x, never headlined). The fix would be
combined native setters (`messageSetInt` et al. in one crossing, no
value malloc), i.e. new C entries plus a second setter family in
`lib/pbwire.dart` for a non-shipped arm: disproportionate, not a
round. The `shim` row stays the native-object answer.

## Cell: Julia tree decode at small (plan 2026-11-22 Task 6, static-link group)

Worst red cell on a non-shipped arm: `tree` decode 0.18x at 136 B vs
ProtoBuf.jl (212.5 vs 1181.2 MB/s; the shipped `shim` row is 0.93x on
this tier and above the baseline everywhere else). Micro-split on the
same host (`pbench/micro_ab.jl`, since removed; best of 5):
decode-only 127 ns/op (matches the one-`ccall` shim decode at ~123
ns), read-only by name 492 ns/op, read-only with hoisted `Field`
objects 132 ns/op, full `tree_decode` 733 ns/op (~127 + 492 +
re-encode).

Mechanism, verified by the hoisted-Field A/B: each by-name get pays a
`Dict{String,Field}` lookup (~10 ns, `field()` is already cached per
`MsgType`) plus two ccalls (`pbw_message_get_field` +
`pbw_value_get_*`, ~4 ns each), i.e. ~18 ns per field against ~8 ns
hoisted, times ~30 field reads. Hoisting moves the row but redefines
the timed task (the arm prices the shipped by-name per-field API);
removing the ccalls needs combined native getters, i.e. a new C entry
for a non-shipped arm: disproportionate, not a round. The `shim` row
stays the native-object answer; the small-tier `tree` decode cell
stays flagged, same class as the ledger's Julia 136 B decode entries
above (per-call malloc/free plus the boundary, fixed only by an
ownership or API change).

## Kept (Node shim): leaf-table single-pass decode (Task 4, 2026-11-24)

`decodeToObject` validated every range with `scan_range` and then
materialised it with `slices_to_js`, which allocates four scratch arrays per
message level: 3000 `Inner3` payloads paid 12000 `calloc`/`free` pairs plus a
second C scan per nested message. Tables with only singular
scalars/strings/bytes (no repeated, oneof or sub-message field) now validate
and materialise in one tag scan (`wire_leaf_to_js`, flagged once per
descriptor in `table_build`): later occurrences overwrite, which is last-wins
for a table with no oneof and no merge. Semantics pinned by a new
`decodeObject matches the tree decoder on wire edges` block in
`bindings/node/test/test_pbwire.js` (last-wins, merge, oneof reset,
packed/unpacked/mixed, packed-for-singular skip per core parity, unknown
skip, closed-enum absence, truncated/malformed/deep-nesting throws, missing
required) plus a 563-payload ad-hoc differential of `decodeObject` against
tree `get()` with zero divergences; the suite is green before and after.

A/B (`bindings/node/benchmarks/ab.sh`, `AB_REQUIRE=decode`, interleaved,
best-of-rounds): xlarge decode median 1.09-1.10x across three sessions
(5/5, 5/5, 4/5 rounds; the one lost round ran under ~30% host inflation),
large 1.03-1.09x, small/medium inside the noise band (1.00-1.03x), encode
untouched everywhere (0.98-1.03x medians). Formal `make node-bench`
(`results/bench-node-20261124T193829Z.json`): xlarge `shim` decode 57.9 MB/s
against protobufjs 429.7 (0.13x, was 0.13x), encode 56.3 against 297.0
(0.19x, was 0.18x).

Encode deliberately untouched: `js_to_message` already builds the tree in the
type's arena (the kept 2026-11-21 change, +16% xlarge); what remains per field
is one `napi_get_named_property` plus one property enumeration per object
(the unknown-field reject), i.e. the ~60-70 ns N-API read floor the README
names. Direct-to-wire emission would save only the ~43 us C tree walk (~3%
of the op). The honest ceiling stands: the C core itself encodes the xlarge
payload at ~1628 MB/s (`proxy` row, 5.5x protobufjs), so the whole 0.19x /
0.13x gap is the ~18k encode / ~30k decode N-API interactions, and no
in-addon tuning crosses 1.0x against an in-heap generated runtime. Both
timer arms stay deep and fresh-task (the fold A/B moves neither side), so the
pair is fair and the cells stay red as documented.

## Kept (Java core): direct-linked table setters + decode dispatch trims (Task 5, 2026-11-24)

Pre-change formal `results/bench-java-20261124T135431Z.json`: xlarge `core`
encode 436.1 MB/s against protobuf-java 460.8 (0.95x), decode 334.0 against
418.7 (0.80x). Encode was already at parity in quiet runs; decode was the
red cell, so all changes below are decode-side (encode paths untouched).

Profile (throwaway `/tmp` probes against the bench classes, then JFR
`ExecutionSample` on the xlarge decode loop): the decode pays ~18k Builder
setter/adder calls per iteration (3000 packed nums + 3000 unpacked tags +
3000 packed flags + 3000 inners x setName/setValue/addInners + singulars),
and every one crossed a `MethodHandle.invoke` adaptation frame
(`Bind.lambda$intSetter$0` / `lambda$setter$0` on the hot stacks), plus a
per-call guard frame, plus `ArrayList.grow` on the four fresh 3000-long
Builder lists, plus a `HashMap` alloc + hash + entry walk for the single
pending singular sub-message, plus a kind `switch` per packed element.
Encode getters were already direct method references, which is why encode
was never red in quiet runs.

Kept (all in `bindings/java/runtime`, no generated-file change, so
Kotlin/Scala/Clojure inherit them with no code change on their side):

- `Bind` setters spin direct calls via `LambdaMetafactory` instead of
  per-call `MethodHandle.invoke` (same signatures, same loud bind-time
  failure; non-linkable targets keep the old reflective path; runtime
  setter failures on the fast path now propagate raw, documented in the
  binding README). Isolated probe decode 340.4 -> 434.9 MB/s (+28%).
- `parseInto` keeps a single pending singular sub-message in two locals
  and only promotes to the `HashMap` on a second distinct field (+2%).
- Packed int/long loops hoist the kind `switch` out of the element loop;
  the per-call guard frame is dropped; the unpacked scalar wire check
  reads the precomputed `FieldMeta.wt` instead of the `singularWt`
  switch (+4% combined).
- Harness fairness (`bindings/java/benchmarks/Bench.java`): the baseline
  decode arm folds every gate-checked field (`consumeBaseline`, same live
  sink, every repeated element like the tree arm's new full fold) instead
  of timing a bare parse (fairness §4). Measured nil on both sides:
  protobuf-java strings parse eagerly (`readStringRequireUtf8`), so the
  fold costs it nothing — kept as documentation, not as a speedup.

A/B (isolated `/tmp` probes, same session, alternating class dirs):
decode 340.4 -> 489.6 MB/s against a folded baseline at ~480-492
(~1.0x in isolation). Formal `make java-bench`
(`results/bench-java-20261124T200358Z.json`, loaded host): xlarge `core`
encode 443.9 vs 445.0 (1.00x, was 0.95x), decode 348.1 vs 416.6 (0.84x,
was 0.80x). `make java-test` green (TestGenerated 77 -> 97 checks: new T7
pins typed decode edges — scalar last-wins, singular-message merge,
packed/unpacked acceptance, packed-for-singular skip vs the oracle,
typed unknown-field drop vs the dynamic tree's retention, field-zero /
stray-end-group / truncated-tag / truncated-fixed32 /
truncated-length-delimited throws, 300-deep group nesting hitting the
256 cap, missing-required on decode); kotlin/scala/clojure test gates
green on the shared runtime.

Rejected / measured (with reasons):

- ASCII string fast path on decode (Latin1 inflate behind an ASCII
  scan): ~1.5 ns per small string either way — both sides already pay a
  `String` construction per value, so there is no gap to close. Not built
  in the repo (scratch probe only).
- Anything inside the generated `Builder`/message storage: the ~2900
  `Integer` boxes on packed-nums decode, the four list copies plus four
  `unmodifiableList` wrappers plus the 3000-inner `isInitialized` walk
  in `build()`. protobuf-java keeps primitive `IntArrayList`s and flips
  them immutable zero-copy. Reaching that storage needs per-message
  primitive lists from codegen plus a regen of every Java model — out of
  the runtime-only scope, and disproportionate for the ~10% it prices at.

Honest ceiling: decode 0.84x on the loaded formal bench, ~1.0x isolated.
The encode cell is green (1.00x); the decode cell stays red and is now a
floor with its mechanism named above. No further untried one-change
runtime mechanism on the JVM `core` decode path.

## Cell: Objective-C `tree` xlarge encode (0.97x) — confirmed noise band, no change

Task 9 of the below-baseline plan (2026-11-24): five consecutive
`make objc-bench` runs on the same machine keep xlarge encode below the line
without ever crossing it — 0.964x, 0.984x (prior formal runs), then 0.858x,
0.981x, 0.972x (three runs back to back; the 0.858x pass ran under load from
other benches on the same host). Decode stays green throughout
(1.16x-1.20x). Formal pasted run:
`results/bench-objc-20261124T203820Z.json`.

A probe built from the same harness sources times the fresh-build half alone
(xlarge pad, 2000 iterations): `fill_pbwire` ~374 us/iter vs `fill_official`
~407 us/iter vs the shared `padded(74775)` fixture alone ~361 us/iter. The
74 KB `NSString` both sides rebuild identically dominates the ~368 us op; the
encode tail itself (held-tree re-encode, no build) is ~1 us. So there is no
asymmetric buffer-reuse cost to align with the C harness
(`bindings/c/benchmarks/bench_struct.cc` reuses one output buffer per side):
both ObjC sides already allocate exactly one fresh output buffer per
iteration, and reusing it would only touch the ~1 us tail, not the ~360 us
shared fixture both sides pay inside the timer. No binding or harness change
shipped; the residual 2-3% lean is dispatch wobble inside a tier both sides
build identically, inside the few-percent run-to-run band the harness
documents. Revisit only if a quiet-machine run ever shows a gap clearly
outside that band.

## Cell: Lua `shim` xlarge encode (0.98x) — confirmed tie, no change

Task 10 of the below-baseline plan (2026-11-24): three consecutive
`make lua-bench` runs on the same host keep xlarge `shim` encode just under
the line without ever opening a real gap — 0.98x, 0.99x, 0.98x (prior formal
run 0.99x; the other encode tiers sit green at 1.03x-1.06x in the same runs).
Decode stays green throughout (1.42x-1.47x). Formal pasted run:
`results/bench-lua-20261124T204559Z.json`.

No probe was needed beyond the re-runs: unlike the ObjC case there is no
candidate asymmetry to isolate — the `shim` path already encodes straight
from the Lua table into the schema context's reused buffer in one C entry
per op, which is exactly the C-harness buffer-reuse shape
(`bindings/c/benchmarks/bench_struct.cc`), and both sides pay the same
fresh-table construction inside the timer. No binding or harness change
shipped; the residual 1-2% lean is dispatch wobble inside a tier both sides
build identically, inside the ±10% run-to-run band the binding README
documents. Revisit only if a quiet-machine run ever shows a gap clearly
outside that band.

## Test-environment history (from BENCHMARKS.md)

Moved verbatim from the BENCHMARKS.md top matter on 2026-11-25 (trim);
the tables above it are the current numbers.

> **Optimization (2026-11-14):** the proto3 `string` UTF-8 validation that
> runs at parse time (see
> [#aligned-cost-proto3-string-utf-8-validation](#aligned-cost-proto3-string-utf-8-validation))
> was the main reason the decode rows below had fallen behind. It now shares
> one sequence rule and skips ASCII runs 16 bytes at a time, so a 74 KB value
> costs ~1.3 us instead of ~20 us and a short value is checked inline (measured
> in the ledger). The tables below are the post-change runs; the remaining
> decode gaps are baseline strength, not validator cost.

Last full refresh: 2026-11-24, 13:46-14:48 UTC (every `make *-bench`
on this tree except Erlang, one harness at a time, the whole matrix in ~60 minutes;
formal runs `results/bench-<lang>-20261124T13*.json` / `T14*.json`; dart's first
attempt failed on a missing `protoc-gen-dart` and was re-run with
`~/.pub-cache/bin` on `PATH`), plus an
08:30 UTC core re-run after `bench/bench.cc` switched the generated and
DynamicMessage encode arms to fresh-build per iteration (same construct +
serialize calling method as the pbwire arms; see the Core section). Core
re-run 2026-11-24, 07:19 UTC: the pbwire arms now go through the generated C
struct and the compiled struct codec (thin model, same new + set + encode task
as the baseline; formal run `results/bench-core-20261124T071914Z.json`).
Core re-run 2026-11-24, 07:38 UTC: the plain arms go through a fresh arena per
call (same batched profile as the reference parse; formal run
`results/bench-core-20261124T134623Z.json`). Go-parity re-time 2026-11-21/22 (issue 41 plan): every binding
with a `pbench/` re-times its `tree` arm as the native-in/native-out full task
(fresh native object per iteration, alloc and free inside the timer, deep
materialization of every gate-checked field); `shim` rows and the shipped JVM
`core` rows keep their timers except where the task
changed. Pbench-only arms (the prebuilt `proxy` task, pure-language `core`
ports, Go `dync`, the vendored `cshim`, the JNI ceiling) stay in
`bindings/<lang>/pbench/README.md` and are never pasted here. The re-timed runs are `results/bench-<lang>-20261124*.json` (one
binding at a time under `results/.bench.lock`); the per-binding tables live in each binding's README (`## Benchmarks`), and
the at-a-glance rows and the per-binding summary below are pasted from them.
Full re-time 2026-11-24 (fresh-build headline contract, `docs/perf-fairness.md`):
every binding re-ran once under `results/.bench.lock`; the per-binding tables,
the at-a-glance rows and the per-binding summary below are pasted from
`results/bench-<lang>-20261124*.json`.
Partial refresh 2026-11-15, 02:26-03:12 UTC: core, Python
and Julia re-ran after the first generic pass (plain-encode `wire_len` hint,
32 inline Python slices, sealed-arena implicit reset), then Java, Kotlin,
Scala, Clojure and Julia again plus core and Python once more after the
follow-ups (arena reset keeps the used chunk prefix, `Message` drops its
`AtomicBoolean`, decode drops its `ThreadLocal` reader, Scala measures
interleaved), and finally 04:00-04:30 UTC core, C, Python, Julia, Go, Rust,
Node, Ruby, Lua, PHP and Zig re-ran once the encoder cached its buffer base
pointer and capacity (a C-only change, so Java, Kotlin, Scala and Clojure kept
their 03:00 numbers). The at-a-glance rows, the per-binding summary and the
core table reflect those runs, except where a re-time above
re-pasted a binding's table from its `results/bench-<lang>-20261124*.json`
run. Each row is pasted verbatim from one run of that binding, so
both columns of a row come from the same measurement; where a binding
was re-run (Ruby, Objective-C, Kotlin, Elixir, Julia, Erlang) the pasted run is the one
whose own baseline did not collapse under host load (a run whose fastest
baseline is under 85% of that binding's median baseline is treated as
contaminated and skipped). The host carried someone else's background load
throughout (load average 5-14 on 14 cores, ~0.4-1.0 normalised), so absolute
MB/s reads 10-20% low against a quiet machine while `x baseline` reproduces
(run-to-run variation is ±10% per cell, more on the JVM narrow tiers).
Erlang ran from its own record this time
(`results/bench-erlang-20261124T110417Z.json`), pasted like every other binding.
Dart ran with `protoc-gen-dart` 25.1.0 on `PATH` (`dart pub global activate
protoc_plugin` plus `~/.pub-cache/bin`).

| piece | version |
|---|---|
| machine | Apple M4 Pro, 14 cores, 48 GB RAM |
| OS | macOS (Darwin 27.2.0, arm64) |
| C/C++ | Apple clang 21.0.0, `-O2` |
| `protoc` / libprotobuf C++ | 36.2 |
| Python | 3.15.2, `protobuf` 7.36.2 (upb) |
| Erlang/OTP | 29.0.1 |
| Node | v26.3.0 (`google-protobuf` 4.0.3, protobufjs 8.8.1) |
| Go | 1.27.3 |
| Java | OpenJDK 26.0.1 |
| Ruby | 4.0.9 (`google-protobuf` gem 4.36.2) |
| Objective-C | same clang, Xcode Foundation |
| PHP | 8.5.12 (`protobuf` C extension 5.36.2) |
| Dart | 3.13.7 |
| Rust | 1.99.1 |
| C# | .NET 10.0.403 |
| Lua | 5.5.1, `lua-protobuf` 0.6.0 |
| Swift | 6.4.2, `swift-protobuf` 1.39.0 |
| Scala | 3.10.0, ScalaPB 0.11.18, `protobuf-java` 4.36.2 |
| Elixir | 1.20.5 / OTP 29, `protobuf` 0.17.1, `pbwire` 0.1.7 (Hex) |
| Zig | 0.17.1 (`-Doptimize=ReleaseFast`), zig-protobuf 7.0.0 |
| F# | .NET 10.0.403 (Google.Protobuf baseline via C#) |
| Clojure | 1.12.0, `protobuf-java` 4.36.2 |
| Julia | 1.13.3, ProtoBuf.jl 1.3.0 |
| R | 4.6.2, RProtoBuf 0.4.28 |

## Linux (docker) paired run, 2026-11-14 (from BENCHMARKS.md)

Moved verbatim from the BENCHMARKS.md top matter on 2026-11-25 (trim).

Every table above is Apple-silicon. The claims have to hold on the other
architecture too, so `make bench-docker` runs the same harnesses — same
payloads, same baselines, same measurement standard — inside
`docker/bench.Dockerfile`, and `make bench-compare` pairs the newest record
per binding/platform in `results/`, row by row:

```bash
make bench            # the macOS side (or whatever host you are benching)
make bench-docker     # the Linux side: core, c, go, rust, node, ruby, lua, java, python
make bench-compare    # newest pair per binding, xlarge tier, speedup per row
```

The image installs the pinned protobuf (so the generated baseline is the same
version as the tables above) plus the Go, Rust, Node, Ruby, Lua, Java and
Python toolchains. It covers what runs without an Apple toolchain:
Objective-C and Swift stay host-only, PHP needs the official C extension as its
baseline (Ubuntu packages none, and building it from source needs a generated
`utf8_range` amalgamation only protobuf's own build produces — a pure-PHP
baseline would be a ~300x different comparison), and the toolchains the image
does not install (Dart, Zig, C#/F#, Julia, R, Erlang/Elixir,
Kotlin/Scala/Clojure) run on the host only. `--bindings core,c` narrows a run,
`--rebuild` rebuilds the image, and the container always benches a *copy* of the
tree, so ELF objects never land in the host's `build/`.

Last run: 2026-11-14, `make bench-docker` on the image in the repo, `make
bench-compare` for the pairing. Absolute MB/s per row is what `--absolute`
prints; the speedup against the same baseline on the same side is what travels.

### core

- macOS: 2026-11-14T18:00:19+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:30:14+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.42x (pb gen) | 1.14x (pb gen) |
| xlarge | decode | 1.78x (pb gen) | 2.31x (pb gen) |

### c

- macOS: 2026-11-14T18:19:31+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:31:26+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.32x (pb gen) | 1.32x (pb gen) |
| xlarge | decode | 1.96x (pb gen) | 2.82x (pb gen) |

### go

- macOS: 2026-11-14T18:20:12+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:32:08+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 2.61x (protobuf-go) | 2.65x (protobuf-go) |
| xlarge | decode | 2.38x (protobuf-go) | 2.27x (protobuf-go) |

### python

- macOS: 2026-11-14T18:21:09+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:38:46+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode(data) | 7.11x (pb upb) | 8.05x (pb upb) |
| xlarge | decode(bytes) | 1.17x (pb upb) | 1.28x (pb upb) |
| xlarge | to_dict(bytes) | 3.70x (pb upb) | 4.73x (pb upb) |
| xlarge | encode(parsed) | 63.05x (pb upb) | 55.86x (pb upb) |

### java

- macOS: 2026-11-14T18:01:43+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:37:47+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.09x (protobuf-java) | 0.96x (protobuf-java) |
| xlarge | decode | 1.94x (protobuf-java) | 1.72x (protobuf-java) |

### node

- macOS: 2026-11-14T18:22:24+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:35:33+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 2.99x (protobufjs) | 4.91x (protobufjs) |
| xlarge | decode | 2.77x (protobufjs) | 2.37x (protobufjs) |

### ruby

- macOS: 2026-11-14T18:44:34+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:36:19+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.19x (google-protobuf) | 1.11x (google-protobuf) |
| xlarge | decode | 1.69x (google-protobuf) | 1.89x (google-protobuf) |

### rust

- macOS: 2026-11-14T18:03:19+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:33:27+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

skipped 2 row(s): the baseline differs across sides (prost vs protobuf-rust, protobuf-rust vs prost)

| case | op | macOS x baseline | Linux x baseline |
|---|---|---|---|
| xlarge | encode | 1.04x (prost) | 1.49x (prost) |
| xlarge | decode | 1.92x (prost) | 1.10x (prost) |

### lua

- macOS: 2026-11-14T18:31:09+00:00 (Darwin 27.0.0, arm64, Apple M4 Pro, 14 cores, libprotoc 36.2)
- Linux: 2026-11-14T06:36:57+00:00 (Linux 6.8.0-64-generic container, aarch64, ?, 4 cores, libprotoc 36.2)

no comparable row for xlarge


## Measurement notes: the core fast path (from BENCHMARKS.md)

Moved verbatim from the BENCHMARKS.md top matter on 2026-11-25 (trim).

The decoder's generic length reader (`pbw_len_read`, in `wire.c`) is an
out-of-line call. Strings and bytes shorter than 128 bytes — most of them —
now take a one-byte inline path in `read_length()` instead. A/B measured with
two bench binaries built from `HEAD` and the change, 8 alternating runs of
200k iterations, median ns/op:

| case | HEAD decode | now decode | delta |
|---|---|---|---|
| 136 B | 131.6 | 127.0 | -3.5% |
| 174 B | 196.2 | 193.0 | -1.6% |
| 2469 B | 4998.9 | 4862.9 | -2.7% |
| 74913 B | 139641.8 | 140036.4 | +0.3% (noise) |

Encode paths are untouched by this change (they moved by the same +/-3% noise
floor, which is how the run was calibrated). Same session, the same change was
rejected for `skip_value()` (unknown fields): no measurable effect, so that
call stays out of line.

The decoder used to inline fast paths only for one-byte tags and run everything
else through per-item generic calls; two-byte tags carried a second copy of the
inline bodies. All tag sizes now share one link-time action switch (the action
lives on the field, covering singular and repeated shapes), repeated appends
reuse a cached list slot instead of the get-or-create call, and packed runs
append through reserved list storage with no call per item. Measured two ways:
a C best-of-5 of decode+free on the 74913-byte matrix3 payload went 141.1 us to
107.4 us (-24%), and the PHP binding's full-xlarge decode over 10 interleaved
rounds against the official C extension moved from 0.91x to 1.10-1.19x. Packed
encode got the same treatment (one dispatch per run, blind kind-specific write
loops over a hoisted cursor): packed bool x3000 went 4.8 us to 1.1 us, packed
int32 x3000 7.0 us to 3.5 us, both best-of-5. Unpacked-item loops were tried
hoisted the same way and reverted: the bigger `encode_message` cost more in
icache and register pressure than the per-item switch saved, measured -33% on
the core bench's unpacked-heavy message.

Formatting/parsing doubles used `strtod`/`snprintf`, which follow `LC_NUMERIC`:
a host that calls `setlocale(LC_NUMERIC, "de_DE.UTF-8")` made the JSON parser
read `1.5` as `1` and the printers emit `1,5` (invalid JSON). `wire.c` now owns
both conversions (`pbw_strtod`, `pbw_format_g`): the print/parse entry points ask
`pbw_locale_dot()` once, the C-locale path is the same libc call as before and
is inlined, and only a non-'.' separator takes the translating slow path.
Measured with the same A/B method (14 alternating runs, 400k iterations): JSON
parse +0.4%, JSON print +0.7%, text parse +0.3%, text print +0.1%, with the
unmodified reference rows drifting -0.7..+0.5% in the same runs, i.e. inside the
noise floor. The same pass removed the JSON parser's 64-byte number buffer (long
numbers were rejected; the reference has no such limit).

The JSON printer built a per-message `order` array (malloc + insertion sort)
before printing fields, although a linked schema's fields are already sorted by
number (as `link_message()` leaves them). The linked path now prints in place:
one pass, no allocation. Measured two ways, medians of alternating runs:

| measurement | before | after | delta |
|---|---|---|---|
| `make bench` json print (one message) | 1815.3 ns | 1746.5 ns | -3.8% |
| `pbw_json_print` on matrix3 x3000 (3000 messages) | 13.3 ms | 12.6 ms | -5.2% |

The text parser also stopped malloc-ing a buffer per numeric token (stack
buffer first); measured neutral on the bench schema (text parse -0.2%), kept
because it removes one allocation per number.

`pbw_pool_release` grew its idle array with one `realloc` per release, so filling
a pool of N contexts cost O(N^2) copying. It now doubles the slot array. Filling
300 pools x 2048 contexts (acquire all, release all, free the pool):

| build | run 1 | run 2 | run 3 |
|---|---|---|---|
| HEAD | 19.7 ms | 20.0 ms | 19.6 ms |
| now | 13.5 ms | 11.9 ms | 11.8 ms |

The rest of the review (overflow guards in the schema, stream and binding
growth paths, and freeing a nested decoded root's arena) only touches failure
and teardown paths. Re-measured with the same A/B method afterwards: every
row inside the +/-3% noise floor (stream `fed`/`detenc` -1..-4%, `sink`
-0.7%, `framed` -0.9%), so nothing was kept that costs throughput.

Decoding materializes a tree, and two structural costs sat on that path: a
decoded string or bytes item took two allocations (the value struct and its
payload), and every decoded item crossed a translation unit into `message.c`
(`pbw_value_new_uint_a`, `pbw_value_new_bytes_a`, `pbw_value_new_message_a`,
`pbw_value_list_push`, `pbw_message_new_a`) — `make` compiles each `src/*.c`
with no LTO, so those were real calls, one or two per decoded value. Three
changes removed them:

- an arena string/bytes value puts its payload in the same bump as the value
  struct (`pbw_value_new_bytes_a`); the heap path keeps its two allocations,
  which `pbw_value_free` releases separately;
- a packed varint run allocates all of its item nodes in one bump, using the
  item count the >64-byte pre-scan already computed; a packed closed-enum run
  keeps the per-item node path (no scan, no node block), because it drops
  unknown values and the block would hold nodes nothing uses. A/B against the
  scan+reserve form on a 100k-value packed closed enum (C probe,
  `pbw_decode_ctx` + reset, median of 15, three interleaved rounds) put every
  row inside the +/-3% noise floor (all-known 442 vs 442 ns, mixed-unknown 578
  vs 583 ns at 100/125 KB), so this one is allocation locality, not a
  throughput win, and the smaller diff was kept;
- the tree builders are `PBW_INLINE` in `internal.h` (the same pattern as
  `pbw_arena_alloc` and `pbw_find_field`). They stay one definition, moved and
  not copied, so a translation unit that never calls one emits nothing for it.

A/B measured on the settled tree with two binaries built from the same sources,
one with the change reverted, alternating in one session (C probe, best-of-5,
`bench.Msg` against the generated payload, `pbw_decode` + `pbw_message_free`,
median ns per decode; 60000 iterations at 132 B, 20000 at 169 B, 2000 at
2755 B, 400 at 85933 B):

| case | before | after | delta |
|---|---|---|---|
| 132 B | 101.1 | 100.1 | -1.0% (noise) |
| 169 B | 158.3 | 155.1 | -2.0% |
| 2755 B | 4076.6 | 3713.3 | -8.9% |
| 85933 B | 119472.8 | 107836.5 | -9.7% |

The win scales with the number of decoded values, which is where the two costs
were; a small message is dominated by the one tree allocation and the call
overhead into the API instead. Encode is untouched: `encode.o` built from the
reverted tree and from the changed tree is byte-identical, and the probe's
encode rows stayed inside the noise floor in every round. An LTO build limited to
`message.c`, `decode.c` and `wire.c` measured the same decode gain on the
same tier, which is what identified the cross-TU call cost; LTO as a default
build flag would change every binding's link step, so the helpers moved
instead.

Packed *fixed-width* runs still allocated one node per item: the list was
reserved (the payload length divides exactly by the item width) but the nodes
were not batched. `read_packed()` now takes the same one-bump node block for
fixed-width runs that the varint scan enables, which covers packed double /
fixed32 / sfixed / float arrays. A/B on a hand-built message with one packed
field (generic shape, not the tier message), alternating runs, best-of-5,
decode+free, median ns:

| case | before | after | delta |
|---|---|---|---|
| packed double x3000, 24 KB | 8666.2 | 6815.0 | -21.4% |
| packed fixed32 x3000, 12 KB | 7383.3 | 6163.9 | -16.5% |
| packed double x64, 515 B | 221.7 | 188.0 | -15.2% |
| packed double x8, 66 B | 53.2 | 51.3 | -3.6% |
| packed sint32 x3000 (control, same varint path) | 11974.9 | 12183.3 | noise |

Three candidates were measured and rejected in the same pass:

- **3-byte varint window in `read_varint()`** (the 1- and 2-byte windows are
  inline already): -4..5% on the 85933-byte tier, but +3..5% on 132 and 169 B,
  where the extra inline copy costs more in the per-field dispatch than the
  call it removes. Same conclusion as
  [the Julia-cell note](#cell-julia-136-b-decode-092-095x); the small tiers are the
  common real case, so it stays out.
- **Presence-bitmap hoist in `decode_fields()`** (keep one word in a register
  and flush at the end): measured with a write-skipping ablation, which is
  upper-bound only - 132 B decode moved -3% plain / -0.6% ctx, i.e. at the
  noise floor, and the hoist would need every storing path to cooperate
  (`pbw_message_put`, `pbw_message_list_for`). Not built.
- **`pbw_ctx` retention for trees above the 1 MiB budget**: raising the cap to
  8 MiB on the 85933-byte tier changed nothing, so the arena's growth
  mallocs are not a cost worth touching; the documented knob is enough.

## Below-the-line cells (from the BENCHMARKS.md top matter)

Moved verbatim from the BENCHMARKS.md top matter on 2026-11-25 (trim).

Every binding with a
wired baseline is re-timed Go-parity (2026-11-21/22, C-side 2026-11-23): every published row
times the same native-in/native-out task on the same payload, never a tree
built before the timer; bytes are identical everywhere. The headline rows now
time the same task the generated baseline times -- one native object in, one
codec call -- so the sub-1.0x cells that remain are codec gaps, not boundary
floors: Node's `shim` (0.19x / 0.13x of protobufjs at
74 KB), PHP's `shim` (2.94x encode / 2.81x decode of
the official C extension under the same fresh-build task), the JVM `core` decode (Java 0.84x of
protobuf-java; Kotlin 0.77x rides the same
runtime, Scala 0.30x adds the case-class translation on top), Clojure's `core` on both ops (0.26x / 0.74x: generic per-field `set` dominates encode, defensive list copies decode), the managed materialization on C#/F# `shim` decode at
74 KB (0.77x / 0.74x: the C decode is faster than the reference parser, but
materializing 3000 managed objects costs more than its whole run), and
Julia's small-tier `shim` encode (0.01x at 136 B: the fresh image build
in Julia dominates the timer; the xlarge 22.35x is the ProtoBuf.jl
baseline collapse below), and ObjC `tree` encode
(0.97x: a fresh per-field build plus alloc/free inside
the timer; the ObjC xlarge tier
rebuilds its 74 KB `str` fixture per iteration). The core's old `tree` headline
(0.39x-0.58x) retired with the fairness contract: the tree-API build price
(malloc + field lookup per value vs direct stores) is no longer a headline. The Lua xlarge `shim` encode cell (0.98x) is a
run-to-run tie, and so is Go's xlarge decode-par (0.99x, serial twin 1.33x).
Julia's, Kotlin's and Scala's xlarge `shim`/`core`
encode ratios (22.35x / 27.74x / 211.33x) are baseline collapses, not codec wins:
ProtoBuf.jl's generated encoder falls to 11.3 MB/s there, `protobuf-kotlin`
to 17.6 MB/s and ScalaPB to 1.2 MB/s. Each cell is inside
the run-to-run band of its own baseline; `docs/perf-attempts.md` records what
was tried there.

Read it as: the in-process core sets the pace (2.1x the C++
generated code on encode at this size, 1.9x on decode), and every headline row times the same task as
the generated baseline -- one native object in, one codec call -- so the
below-the-line cells are codec gaps, not boundary floors: Node `shim`
(0.19x / 0.13x of protobufjs),
PHP `shim` (2.94x / 2.81x of the official C extension), the
JVM `core` decode (Java 0.84x, Kotlin 0.77x, Scala 0.30x) and Clojure's
`core` (0.26x / 0.74x).
The above-the-line cells are the same task won outright: the core thin
model 2.1x / 1.9x, Ruby `shim`
5.03x / 6.11x, PHP `shim` encode 2.94x, Rust `shim`
2.02x / 1.86x, Dart 1.82x / 4.84x, C# `shim` encode 1.32x, Swift 3.24x / 2.54x, Zig
1.84x / 1.47x. The outliers are baseline or unit effects, not pbwire
effects: Python's encode column is the dict path (3000 sub-messages, 12000
ints), the xlarge encode columns of Julia, Kotlin and Scala are baseline
collapses (ProtoBuf.jl's
generated encoder falls to 11.3 MB/s there, `protobuf-kotlin` to 17.6 MB/s,
ScalaPB to 1.2 MB/s), and the Objective-C tier is a
memcpy race over one 74 KB string (the 16-byte UTF-8 word scan is what keeps
its decode near the copy cost).
The headline names the most-popular baseline: Node's native rows are quoted
over protobufjs, and Node's `shim` row -- the native object in and out --
is 0.19x / 0.13x of protobufjs: the same task on both sides, so the gap is
the codec's, not the boundary's. PHP's native rows are over the official C
extension (the pure-PHP runtime trails it by ~360x / ~110x at 74 KB) -- see
their sections. The old per-field `tree` headlines are retired: the published
rows are the struct-passing headline arm, and per-field timings stay in
`bindings/<lang>/pbench/README.md`.

## Cell: C#/F# shim decode at xlarge (perf loop Task 3, .NET — closeout 2026-11-25)

Pre-round formals on this host: C# xlarge shim decode 381.3 vs 493.0 MB/s
(0.77x, `results/bench-csharp-20261124T192025Z.json`), F# 340.6 vs 462.4
(0.74x, `results/bench-fsharp-20261124T191738Z.json`); large decode red on
both (0.72x/0.73x class), small/medium green. Four idea rounds were measured
against interleaved in-harness baselines; R1+R3 kept, R2/R4 reverted.

Kept R1 — repeated-`string` (Tags) first-element interning: the generated
`ReadImage` decodes element 0 once and reuses the reference while the image
bytes match (`ImageText.Equal`, byte-wise C-string compare in
`bindings/csharp/Pbwire/Pbwire.cs`); anything else falls back to `Decode`.
Strings are immutable so sharing the reference keeps `Equals`/round-trip
identical. Gate: new T-siblings in `GeneratedTest.cs` plus byte-identical
re-encode; `make csharp-test` / `make fsharp-test` green.

Kept R3 — repeated sub-message Inners Name interning (same p0/s0 idiom):
`codegen/internal/gen/csharp.go:csInternChild` fires only for a same-file,
non-map-entry child with exactly one plain string field and otherwise plain
proto3-implicit scalars; the hit path copies the scalar tail inline and skips
`ReadImage`, the miss path keeps `ReadImage`. Pinned by new T8
(repeated-name reference reuse, miss decodes to its own reference, empty-name
edge, byte-identical re-encode).

Rejected R2 — Inners steady-state object reuse (reuse `Inners[i]` in place,
new only on growth): fresh-task `Unmarshal` news the root every iteration so
`Count` is always 0 and the reuse branch never hits; median -2.4% (extra
branch, no win). Reverted, template untouched.

Rejected R4 — `ImageText.Equal` word-compare (strlen once + uint64/uint
head fast path): slower on both xlarge rounds (+1.4%/+0.9%) because every
element re-pays `strlen(p0)` while the payloads are 3 B / 8 B (the saved
byte-loop cycles never cover it). Structural, not tunable. Reverted.

Closeout formals (idle host, one harness at a time):
`results/bench-csharp-20261125T043109Z.json`: xlarge shim decode 160782.0 ns
(465.9 MB/s) vs protobuf-csharp 144431.0 ns (518.7 MB/s) = **0.898x**
(ns ratio 1.113); large 0.878x; small/medium 1.59x/1.55x.
`results/bench-fsharp-20261125T043307Z.json`: xlarge shim decode 161688.4 ns
(463.3 MB/s) vs 146969.7 ns (509.7 MB/s) = **0.909x** (ns ratio 1.100);
large 0.888x; small/medium 1.54x/1.54x.
`make csharp-test` (6464 checks) and `make fsharp-test` (6453 checks) green
on the kept tree.

Floor declared: both xlarge (and large) shim-decode cells stay red at
~0.90x. R1+R3 moved C# 0.77x -> 0.90x and F# 0.74x -> 0.91x by removing the
repeat-value string decodes; what remains is the managed materialization the
2026-11-22 entry already split (3000 strings + 3000 sub-messages into objects
per xlarge decode). No further boundary tuning is proposed — closing it
needs a mechanism change (e.g. dropping the image copy), not another round.

## Cell: Node shim at xlarge (perf loop Task 2, Node — closeout 2026-11-25)

Pre-round formals: Node xlarge shim ~0.19x encode / ~0.13x-0.14x decode of
protobufjs. Two idea rounds were measured against interleaved in-harness
baselines; R1 kept, R2 reverted.

Kept R1 — decode repeated-`STRING` last-value reuse
(`bindings/node/runtime/addon.c:slices_to_js`): caches the last decoded
`PBW_TYPE_STRING` element and reuses the `napi_value` when the next element
has equal length and byte-equal bytes (`memcmp`); miss path keeps
`slice_to_js`. JS strings are immutable so sharing the reference keeps the
round-trip identical. Marginal +1~2% on xlarge/large decode (3/3 medians won,
12/15 rounds), encode untouched, small/medium inside the band. Gate +
`make node-test` (71/71) green.

Rejected R2 — encode drop of the second `napi_typeof` (thread the observed
`vkind` into `set_field`): formal gate 0/5 rounds passed (5/5-wins rule not
met); isolation A/B medians favored the candidate in 27/28 cells at the
predicted small>medium>large≈xlarge shape but round-level drops under host
load (11.8/8.9/10.7) kept it under the bar. Semantically zero-risk (no
intervening JS possible), but unkeepable under the rule. Reverted, R1 kept.

Closeout formals (idle host, one harness at a time):
`results/bench-node-20261125T044958Z.json` (M4 Pro, Darwin 27.2.0), xlarge
(74913 B, 8000 iters): shim encode 1313919.3 ns (57.0 MB/s) vs protobufjs
248776.8 ns (301.1 MB/s) = **0.189x**; shim decode 1267968.3 ns (59.1 MB/s)
vs protobufjs 182737.5 ns (409.9 MB/s) = **0.144x**.
`make node-test` green on the kept tree (71/71 pass).

Exhaustion declared on decode (no further single-variable mechanism left;
the remainder is the ~30k N-API interactions floor); encode R2 stays
reverted under the keep rule. Both xlarge shim cells stay deep red. Closing
either needs a mechanism change (fewer N-API object/value creations per
element, or a different emission shape), not another tuning round.

## Cell: Node flat-image `shim-image` arm (generated-passing perf Task 1, 2026-11-25)

Spike: one N-API crossing per op over a hand-written flat ArrayBuffer image
(`bindings/node/pbench/layout-matrix3.js`, 83-byte fixed header plus a
variable tail in field order) instead of one property read/write per field.
`type.encodeImage(image)` / `type.decodeImage(wire)` in
`bindings/node/runtime/addon.c` walk the image in C through the matrix3
layout table (same field order as `nativeObject`); strings copy into the
type's encode arena exactly like `encodeObject` values, so nothing aliases
the host buffer. Bench arm in `bindings/node/benchmarks/bench.js`
(`pbwire-node-shim-image`): fresh image build per iteration inside the timer
on encode, image-to-object conversion inside the timer on decode — the same
fresh-task shape as `shim`.

Formal `make node-bench` (`results/bench-node-20261125T094543Z.json`;
same-run ratios, so the host's load (~6-16 from other tenants, absolute
MB/s below the idle records) moves both sides together):

| tier | enc image/shim | enc image/pjs | dec image/shim | dec image/pjs |
|---|---|---|---|---|
| 136 B | 2.07x | 1.38x | 2.61x | 0.42x |
| 174 B | 1.82x | 1.13x | 3.08x | 0.39x |
| 2469 B | 1.20x | 0.27x | 2.07x | 0.27x |
| 74913 B | 1.15x | 0.22x | 1.92x | 0.27x |

Rejected: the bar is >= 1.0x vs protobufjs on both ops, and decode sits at
0.27x-0.42x on every tier (encode clears the bar only at 136/174 B). The
crossing elimination is real — the arm beats `shim` on all eight cells
(1.15x-3.08x), removing the ~30k N-API interactions the closeout above
names as the floor — but the image just moves the per-field work into the
JS heap: one TextEncoder.encode per string (6000+ at xlarge) plus DataView
writes on the way in, one TextDecoder.decode per string plus DataView reads
on the way out, with a full tree build in C either way. protobufjs pays none
of that (own-heap objects, no translation, no second representation), so the
gap grows with the repeated-field scale and no tuning of this arm closes a
~2.4x-3.8x decode gap. Gate (`OK shim-image parity` all four tiers,
byte-identical to the reference) and `make node-test` (71/71) stay green;
the arm stays in the harness as a labeled spike row, no headline change.

## Cell: embedded encode x100/x3000 vs nanopb (perf loop Task 5 — exhaustion 2026-11-25)

Round 1 (packed INT32 blind-write + 1B/2B varint fast-path in `write_packed`):
reverted. Same-session alternating A/B (best-of, iters fixed): x100 proxy
multi-round reversed (+2.8% on best-of, 2W-3L), x3000 proxy mixed (3W-2L with
R2/R6 reversed), most round deltas inside the ±5% noise band under host load
~34; did not meet the "both rows same direction, win every round" bar.
`encode.o` text +224 B (5 blind-loop instantiations × ~45 B), tripping the
flash veto. Byte gate (`pbwire == nanopb` every tier), `make test`
(incl. test_embedded), `make difftest` (20000 cases, >2B fallback fuzzed) and
`make format-check` were all green on the candidate — a clean loss, not a
correctness issue. `git checkout -- src/encode.c`, `src/` verified clean.

Round 2 idea pass: exhaustion declared, no new hypothesis. Attribution: the
headline is the fresh task (`build + fill N×new+set + encode`), while the
same run's `pbwire-proxy` (held-tree re-encode) already leads nanopb ~7-8x —
the 43-45% headline gap is tree-build price (per-value `pbw_value_new_*` +
`pbw_message_set` lookup/validate/append vs nanopb's `memset` + direct
struct stores), not writer price. Every remaining writer shape is either a
ledger-rejected repeat (3B varint window, presence hoist, retain-cap,
wire_len write-back, grow minimum, sizing second walk, LTO, initial
512/1024, per-write capacity check removal, unpacked hoist) or touches a
~2-3% non-dominant share. Closing the headline gap needs a mechanism change
(reused tree / batched build / struct-codec path without a tree), not a
single-variable writer tune. No code change kept; no further rounds.

## Cell: Objective-C `tree` xlarge encode (0.97x) and Lua `shim` xlarge encode (0.98x) — confirmed ties, no change (perf loop Task 4, 2026-11-25)

Four consecutive same-host `make objc-bench` runs: xlarge encode ratios
0.997 / 1.002 / 0.982 / 1.009 (decode stays green 1.13x-1.19x). Four
consecutive same-host `make lua-bench` runs: xlarge shim encode 0.967 /
0.995 / 1.015 / 1.015 (decode stays green ~1.40x-1.46x). Both cross 1.0 in
both directions inside the ±10% run-to-run band; the pasted 0.97x / 0.98x
sit inside the measured noise. No binding or harness change shipped.

## Cell: JVM decode slice (perf loop Task 1 — H-α falsified, floor 2026-11-25)

H-α (converge `materialize` per-element `new Wire.Reader` into one leased
Reader per `getList` call, with re-entry fallback): allocation target
achieved (`Wire$Reader` JFR samples 748 -> 0) but timing regressed
14-18% on tree full-readback (SerialGC best 751.7ms -> 920.9ms, G1 736.8ms
-> 856.0ms, all 5 pairs base-wins): one `ThreadLocal.get` per `materialize`
(~3000/walk) costs more than the TLAB bumps it removes. ab.sh dyn rows
0.98x-1.02x (encode 0%, lazy-decode 0%, as predicted — the path never
touches `materialize`). Reverted; `make conformance-java` (2766 binary +
443 text/JSON), java/kotlin/scala/clojure tests all green on both sides.
Per the round-1 probe, the JVM decode slice has no further unguarded
single-variable mechanism: Java/Kotlin headline core is fenced behind
per-message generated Builder storage (boxing, list copies, build walk —
codegen scope), and the tree path's remaining cost is the lazy double-touch
itself. Floor declared; no keeps from this loop on the JVM side.

## Retest note (2026-11-25): Node R2 quiet re-run confirms the rejection

R2 (encode drop of the second `napi_typeof`) was re-run on a quieter host
(5 fresh interleaved rounds, same 5/5-wins gate): 0/5 rounds passed, medians
19/20 cells still favoring the candidate at the predicted
small>medium>large≈xlarge shape. The effect (~1%) sits below this host's
per-round noise floor at load 8-14, so it cannot clear the keep bar here —
not a false kill from the earlier concurrent load. Reverted again, R1
(last-string decode reuse) stands. A third attempt would need load <3 first.
