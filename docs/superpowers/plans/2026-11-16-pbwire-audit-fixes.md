# pbwire audit fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the 7 Must findings from the audit, each with a regression test and a perf comparison.

**Architecture:** One task per finding, in dependency order (codec first, then schema/memory, then build). Each task keeps the wire format byte-identical and proves no slowdown with `make bench` before/after.

**Tech Stack:** C99 core (`src/`), `make test`, `make bench` / `make bench-c`, `scripts/bench-compare.py`, `clang-format`.

**Spec:** The audit report in the session (16 findings, 7 Must). This plan covers the 7 Must items only; Should/Nice items are out of scope.

## Global Constraints

- C99 only for `src/`, `include/`, `tests/`, `bench/`, `conformance/`; no new third-party deps.
- After any C change: `make test` must pass; after touching `include/`, `src/`, `tests/`, `bench/`, `conformance/`: `make format`.
- Every fix ships a failing-then-passing test; no fix without a perf before/after pair.
- Perf rule: `make bench` verifies byte equality first; a fix that changes bytes fails. Record encode/decode MB/s per tier, flag >5% drop as reject.
- Commits: Conventional Commits, header <= 100 chars, English-only ASCII.
- Work on a feature branch, never `master` (pre-commit hook enforces).

## Review Focus

- Hostile wire bytes that half-mutate state then succeed: expect reject-or-clean, never partial-write + OK.
- Unbounded growth on tiny input (stream feed, packed batch): expect LIMIT/NOMEM early, not OOM.
- Locale/thread races on numbers: expect byte-identical output under `LC_NUMERIC=de_DE`.
- Schema build-then-link reorderings: expect error or correct slots, never silent swap.
- Stale cached pointers across realloc: expect generation check, never UAF write.

---

### Task 1: MessageSet partial mutation

**Files:**
- Modify: `src/decode.c` (`read_messageset_item` singular-merge path)
- Test: `tests/test_pbwire.c` (new MessageSet case)

**Interfaces:**
- Consumes: existing `decode_message`, `skip_field`, `pbw_message_add_unknown` semantics.
- Produces: clean merge-or-unknown behavior relied on by Task 7 conformance check.

- [ ] **Step 1: Write the failing test**
```c
/* Set with ext item=4 {x:5}, then broken second item: type_id=4 + truncated varint payload */
assert decode returns PBW_OK with item unchanged and unknown holding exactly the second item;
assert no partial field mutation.
```
- [ ] **Step 2: Run it to verify it fails**
Run: `make test` (or `build/test_pbwire` subset). Expected: FAIL — field mutated and/or unknown duplicated.
- [ ] **Step 3: Implement `read_messageset_item` temp-merge in `src/decode.c`**
Decode payload into temp `nested`, on OK merge/put, on ERR free + `skip_field`; never touch `existing` on failure.
- [ ] **Step 4: Run tests**
Run: `make test`. Expected: PASS.
- [ ] **Step 5: Perf comparison**
Run: `make bench 2>&1 | tee /tmp/after1.txt` vs baseline record in `/tmp/before.txt`; compare with `python3 scripts/bench-compare.py --tier xlarge --latest results` or manual MB/s diff. Expected: byte-identical, <5% delta; record numbers in commit body.
- [ ] **Step 6: Commit**
```bash
git add src/decode.c tests/test_pbwire.c
git commit -m "fix(decode): isolate MessageSet merge on failure"
```

### Task 2: Stream feed cap

**Files:**
- Modify: `src/stream.c` (`pbw_istream_feed`, `pbw_istream_next`)
- Test: `tests/test_pbwire.c` (istream section) or `tests/test_malformed.c`

**Interfaces:**
- Consumes: `pbw_limits.max_bytes` semantics from `include/pbwire.h`.
- Produces: bounded buffering relied on by gateway loops.

- [ ] **Step 1: Write the failing test**
```c
pbw_istream_new(&(pbw_limits){.max_bytes=64}); feed 256 x "\x80"; expect PBW_ERR_LIMIT, len capped.
```
- [ ] **Step 2: Run test to verify it fails**
Run: `make test`. Expected: FAIL (grows to NOMEM, no LIMIT).
- [ ] **Step 3: Implement feed cap in `src/stream.c`**
Check `s->len + len > max_bytes + 5` in `feed`, return `PBW_ERR_LIMIT`.
- [ ] **Step 4: Run test to verify it passes**
Run: `make test`. Expected: PASS.
- [ ] **Step 5: Perf comparison**
Istream has no bench tier; run a micro-loop (feed/next 1k small frames) before/after with `time`; plus `make test` timing. Expected: no hot-path change; record.
- [ ] **Step 6: Commit**
```bash
git add src/stream.c tests/test_pbwire.c
git commit -m "fix(stream): bound istream buffering by max_bytes"
```

### Task 3: Locale-independent numbers

**Files:**
- Modify: `src/wire.c` (`pbw_locale_dot`, `pbw_strtod_slow`, `pbw_format_g_slow`)
- Test: `tests/test_pbwire.c` (JSON/text number section)

**Interfaces:**
- Consumes: JSON/text printer/parser number paths.
- Produces: locale-stable output for Tasks 5-6 conformance checks.

- [ ] **Step 1: Write the failing test**
Set `LC_NUMERIC=de_DE.UTF-8` (skip if locale missing), parse `{"v":3.14}` and print `3.14`; expect stable round-trip across threads.
- [ ] **Step 2: Run test to verify it fails**
Run: `LC_ALL=de_DE.UTF-8 make test`. Expected: FAIL or flaky.
- [ ] **Step 3: Implement locale-free conversion in `src/wire.c`**
Own `.`-only `strtod`/`%g` path or pinned-C-locale cached `dot`; never call `localeconv` per number.
- [ ] **Step 4: Run test to verify it passes**
Run: `make test` + locale variant. Expected: PASS.
- [ ] **Step 5: Perf comparison**
Run: `make bench` (numbers appear in JSON/text only; use existing json/text round-trip timing if present, else `time` a 10k-number print loop). Expected: no regression; record.
- [ ] **Step 6: Commit**
```bash
git add src/wire.c tests/test_pbwire.c
git commit -m "fix(wire): make number parsing locale-independent"
```

### Task 4: Pre-link message generation guard

**Files:**
- Modify: `src/internal.h` (`pbw_message_new_a`), `src/schema.c` (`link_message`), `src/message.c` (get/set/encode guards)
- Test: `tests/test_pbwire.c` (schema section)

**Interfaces:**
- Consumes: `schema->linked`/`frozen` state.
- Produces: generation token checked by Task 5.

- [ ] **Step 1: Write the failing test**
```c
add b=2, a=1; m=new; set(m,2,...); link(); assert get(m,1) is a's value and encode emits field 1.
```
- [ ] **Step 2: Run test to verify it fails**
Run: `make test`. Expected: FAIL (slots permuted).
- [ ] **Step 3: Implement generation check**
Store `linked_gen` in `msg` at create; get/set/encode/decode/`is_initialized` reject on mismatch.
- [ ] **Step 4: Run test to verify it passes**
Run: `make test`. Expected: PASS.
- [ ] **Step 5: Perf comparison**
`make bench` before/after. Expected: one int compare per op, <5%; record.
- [ ] **Step 6: Commit**
```bash
git add src/internal.h src/schema.c src/message.c tests/test_pbwire.c
git commit -m "fix(schema): reject pre-link messages after sort"
```

### Task 5: Stale field pointer guard

**Files:**
- Modify: `src/schema.c` (bump `gen` on add/link), `src/message.c` (`set_field`/`get_field`)
- Test: `tests/test_pbwire.c`

**Interfaces:**
- Consumes: generation token from Task 4.
- Produces: safe cached-field fast path.

- [ ] **Step 1: Write the failing test**
Cache `f=find(1)`, add 100 fields, `set_field(m,stale,v)`; expect `PBW_ERR_INVALID`, no crash under ASan.
- [ ] **Step 2: Run test to verify it fails**
Run: `make test`. Expected: FAIL (wrong slot / ASan UAF).
- [ ] **Step 3: Implement `schema->gen` check in `src/message.c`**
Bump on add/link; `set_field`/`get_field` validate pointer range + generation.
- [ ] **Step 4: Run test to verify it passes**
Run: `make test`. Expected: PASS.
- [ ] **Step 5: Perf comparison**
`make bench` before/after. Expected: negligible; record.
- [ ] **Step 6: Commit**
```bash
git add src/schema.c src/message.c tests/test_pbwire.c
git commit -m "fix(message): validate cached field pointers"
```

### Task 6: Packed batch cap

**Files:**
- Modify: `src/decode.c` (`read_packed`)
- Test: `tests/test_malformed.c` + `tests/test_pbwire.c`

**Interfaces:**
- Consumes: `pbw_limits.max_bytes`, `reserve`/`arena_alloc` paths.
- Produces: bounded decode memory.

- [ ] **Step 1: Write the failing test**
10 MB run of `\x01` for packed int32 with default limits; expect early LIMIT/NOMEM without 240 MB transient (assert via `pbw_ctx_used` or capped reserve).
- [ ] **Step 2: Run test to verify it fails**
Run: `make test`. Expected: FAIL (huge alloc).
- [ ] **Step 3: Implement capped batch in `src/decode.c`**
Only batch when bounded (`max_bytes>0` or `data_len<64KB`); else per-item push / cap reserve at 4096 and grow.
- [ ] **Step 4: Run test to verify it passes**
Run: `make test`. Expected: PASS.
- [ ] **Step 5: Perf comparison**
`make bench` (packed tiers) + `make bench-c` before/after. Expected: small-message path unchanged, large packed within 5%; record per-tier MB/s.
- [ ] **Step 6: Commit**
```bash
git add src/decode.c tests/test_malformed.c tests/test_pbwire.c
git commit -m "fix(decode): cap packed batch allocation"
```

### Task 7: Erlang core sync gate

**Files:**
- Modify: `bindings/erlang/c_src/Makefile` or build to prefer `../../src` (keep vendoring for publish only), `.github/workflows/ci.yml` or `scripts/check.sh` drift check
- Test: new `scripts/check.sh` assertion or CI step

**Interfaces:**
- Consumes: `make erlang-core-sync` behavior.
- Produces: no silent core/NIF divergence for all tasks above.

- [ ] **Step 1: Write the failing check**
Add drift check (diff `src/` vs `bindings/erlang/c_src/core/src/` or build-path assertion); run with stale copy. Expected: FAIL.
- [ ] **Step 2: Implement thin-build default + check**
NIF builds against `../../src`+`../../include`; vendoring only for Hex artifact; wire check into `scripts/check.sh`.
- [ ] **Step 3: Run check to verify it passes**
Run: `./scripts/check.sh` + `make -C bindings/erlang tests` (if OTP present, else check-only). Expected: PASS.
- [ ] **Step 4: Perf comparison**
N/A (build path only); run `make erlang-bench` before/after if OTP present, else note skipped with reason. Record.
- [ ] **Step 5: Commit**
```bash
git add bindings/erlang/c_src/Makefile scripts/check.sh .github/workflows/ci.yml
git commit -m "fix(erlang): build NIF against core, gate drift"
```
