# Contributing to pbwire

## Commit messages (Conventional Commits)

This repository uses [Conventional Commits](https://www.conventionalcommits.org/).

### Format

```
<type>(<scope>): <subject>

[optional body explaining why, not what]
```

- **subject**: imperative and short; the header line is at most 100 characters
- **scope**: required, see the table below
- **language: English only (ASCII)** -- no CJK or other non-ASCII in header or body
  (enforced by `validate-commit-msg.sh`)
- PR titles merged into the default branch follow the same format

### Types

| type | When to use |
|------|-------------|
| `feat` | New feature or module |
| `fix` | Bug fix |
| `docs` | Documentation only |
| `style` | Formatting, no semantic change |
| `refactor` | Restructuring, no external semantic change |
| `perf` | Performance work |
| `test` | Tests only |
| `build` | Build config, dependency versions |
| `ci` | CI, git hooks |
| `chore` | Miscellaneous maintenance |
| `revert` | Revert a previous commit |

### Scopes

| scope | Paths / content |
|-------|-----------------|
| `core` | `include/`, `src/` (runtime, schema, wire codec, descriptor set loading) |
| `protoc` | `include/pbwire-protoc/`, `src/protoc/`, `tests/protoc/` (optional `.proto` importer) |
| `test` | `tests/`, `bench/`, `conformance/` |
| `bindings` | `bindings/` (language bindings over the C ABI) |
| `docs` | `README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `llms.txt`, `LICENSE` |
| `build` | `Makefile`, `CMakeLists.txt`, `cmake/`, toolchain versions |
| `ci` | `.githooks/`, `scripts/`, `.github/` |

When a new top-level code directory appears, add a scope here **and** update `SCOPES` in
`scripts/validate-commit-msg.sh` (the two must stay in sync).

### Examples

```
docs(docs): document local hook setup

feat(ci): add stack detection to pre-push checks
```

## Local validation (shell hooks, no npm)

Enable once per clone:

```bash
./scripts/setup-git-hooks.sh
```

Dry-run the message check:

```bash
echo "docs(docs): test message" | ./scripts/validate-commit-msg.sh /dev/stdin
```

## Build and test

```bash
make test          # build + self-check (required before commit)
make format        # clang-format >= 19 over include/, src/, tests/, bench/, conformance/
make format-check

make difftest      # optional: differential test vs libprotobuf C++ (needs protoc)
PBWIRE_PROTOBUF_SRC=... make conformance   # optional: official binary conformance suite
make bench         # optional: benchmark vs libprotobuf C++

cmake -B build-cmake -DPBW_BUILD_PROTOC=ON   # optional: .proto importer (needs libprotobuf)
cmake --build build-cmake && (cd build-cmake && ctest)
```

**Commit / push gate:** `pre-commit` and `pre-push` reject direct work on `master` and run
`scripts/check.sh` (build + `make test`). CI runs the same checks on Linux and macOS, plus CMake/ctest
and `make format-check`:

```bash
./scripts/check.sh                # run the same gate manually
SKIP_CHECKS=1 ./scripts/check.sh  # bypass, only with a real reason
```

**Note:** this repository uses the shell `commit-msg` hook only. Never add a second validation path
(husky / commitlint / `.husky/`).
