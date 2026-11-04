# pbwire Java binding

`bindings/java/runtime` is the Java binding: a CPython-free, JNI-free FFI layer
over the C ABI in [`include/pbwire.h`](../../include/pbwire.h), using the JDK 22+
Foreign Function & Memory API (`java.lang.foreign`, `Linker.nativeLinker()`,
`SymbolLookup.libraryLookup`). A `FileDescriptorSet`
(`protoc --descriptor_set_out`) becomes a schema at runtime; there is no
generated code and no C glue file.

```java
try (Schema schema = Schema.fromFile("tests/data/sample.desc")) {
    MessageType sample = schema.message("pbwire.test.Sample");
    try (Message message = sample.newMessage()) {
        message.set("int32_field", -42)
                .set("string_field", "alice")
                .set("bytes_field", new byte[] {1, 2, 3})
                .set("numbers", 1).set("numbers", 2);    // repeated: appends
        message.newMessage("inner").set("x", 7);          // nested: created in place
        byte[] wire = message.encode();

        try (Message decoded = sample.decode(wire)) {
            long id = decoded.getLong("int32_field");
            Message inner = decoded.getMessage("inner");
            assert decoded.getList("numbers").equals(List.of(1L, 2L));
            assert Arrays.equals(wire, decoded.encode()); // byte-exact round trip
        }
    }
}
```

## Build and run

```shell
./build.sh          # make shared + javac into bindings/java/out

java --enable-native-access=ALL-UNNAMED \
     -Dpbwire.library=$PWD/../../build/libpbwire.dylib \
     -cp out TestPbwire tests/data/sample.desc     # self-check

java --enable-native-access=ALL-UNNAMED \
     -Dpbwire.library=$PWD/../../build/libpbwire.dylib \
     -cp out Roundtrip tests/data/sample.desc      # example
```

`build.sh` runs `make shared`, not `make`: the FFI `dlopen`s the library, so it
needs `build/libpbwire.dylib` (macOS) or `build/libpbwire.so` (Linux); a static
`libpbwire.a` cannot be loaded. `-Dpbwire.library=<path>` (or `PBWIRE_LIB`)
points the binding at it; without it, `build/libpbwire.{dylib,so}` relative to
the working directory is used. JDK 22 or newer is required, and
`--enable-native-access=ALL-UNNAMED` is required on JDK 24+ (a warning turns
into an error once FFM is blocked by default).

## API

| class | members |
|---|---|
| `Schema` | `fromFile(path)`, `fromBytes(byte[])`, `message(fullName)`, `close()` |
| `MessageType` | `name()`, `newMessage()`, `decode(byte[])` |
| `Message` | `set(name\|number, …)`, `newMessage(name\|number)`, `clear(…)`, `has(…)`, `get(…)`, typed getters, `getList(…)`, `encode()`, `close()` |
| `PbwireException` | `status()` — the `pbw_status`, `getMessage()` — includes `pbw_strerror`/`pbw_schema_error` |

`set` overloads: `long` (every integer, fixed and enum field, raw 64-bit bits),
`boolean`, `double` (a `float` field is narrowed for you), `String` (UTF-8),
`byte[]`. `set` replaces a singular field and appends to a repeated one; there
is no separate `add`. `newMessage` returns a child `Message` that writes
straight into the tree (append for a repeated message field).

Getters: `get(name\|number)` returns the boxed value and `null` when absent;
`getLong/getInt/getBool/getDouble/getFloat/getString/getBytes/getMessage`
return the Java default (`0`, `false`, `null`) when absent; `getList` returns an
empty list. `has` is the presence test — a field that was not set (or not on the
wire) is absent, matching pbwire's explicit-presence model. Enums are read as
integers. A `uint64` above `Long.MAX_VALUE` reads as the same raw bits, i.e. a
negative `long`.

Value mapping: `int32/int64/sint*/sfixed*/fixed*/uint*` → `long`, `enum` →
`long`/`int`, `bool` → `boolean`, `float/double` → `double` (or `Float`/`Double`
from `get`), `string` → `String`, `bytes` → `byte[]`, `message` → `Message`,
`repeated` → `List<Object>` (or `List<Message>` elements).

Errors are unchecked: `PbwireException` for anything the C API reports
(`decode` of malformed bytes, a proto2 `required` field missing on encode, a
load failure whose reason comes from `pbw_schema_error`), `IllegalStateException`
for using a closed schema/message, `UncheckedIOException` for a missing
descriptor file.

Ownership is explicit and double-free safe:

- `Schema.close()` frees the schema; `Message.close()` frees a tree created by
  `newMessage()`/`decode()` and is idempotent (the handle is taken out of an
  `AtomicReference` before freeing, so a `Cleaner` racing `close()` is safe).
- A child `Message` from `newMessage()`/`getMessage()` is a view: it is never
  freed on its own and it throws `IllegalStateException` once its root is
  closed.
- An unclosed root is freed by a `java.lang.ref.Cleaner` when it becomes
  unreachable.

## How the boundary is kept cheap

Every C function has one `static final` `MethodHandle` called through
`invokeExact`, so the JIT inlines the downcall into the wrapper instead of
resolving a signature per call (`invokeWithArguments` boxes every argument and
costs ~40x the native call itself: 144 ns vs 3 ns on the small tier).

On top of that, one workspace per thread holds everything a call needs to hand
back:

| piece | why |
|---|---|
| a `pbw_ctx` | `pbw_encode_ctx` reuses its output buffer, so `encode()` allocates no native buffer and calls no `pbw_free` |
| three scratch slots | the `uint8_t **`/`size_t *` out-parameters every wire call writes through |
| a native input buffer | the bytes crossing into C (`decode` input, `set` payloads), grown on demand so a steady decode loop stops allocating |

Field facts (number, type, label, sub-message type) are resolved through the C
descriptor once per type and cached in a `ConcurrentHashMap`, so `set`/`get` by
name or number cost a map lookup instead of a native call. That makes
`Schema`/`MessageType` safe to share between threads — a schema is read-only
after load — while a `Message` and the workspace stay confined to one thread
(see `TestPbwire`'s 4-thread check).

## Benchmark

`benchmarks/bench.sh` generates `protobuf-java` from the same `matrix3.proto`
with `--java_out` and runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md), printing `ns/op` and `MB/s` for
`pbwire-java` and `protobuf-java` side by side. It fetches `protobuf-java` from
Maven Central once (into `benchmarks/deps/`) and needs JDK 22+ and `protoc`.
All four operations get a warm-up pass, then five interleaved timed passes whose
best is reported, so a busy machine hits both implementations equally.

```shell
./benchmarks/bench.sh      # or: make java-bench
```

The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Copied from [`BENCHMARKS.md`](../../BENCHMARKS.md),
which is the single source for all performance numbers; higher is better, MB/s
over wire bytes:

| payload | op | pbwire-java (MB/s, x protobuf-java) | protobuf-java (MB/s) |
|---|---|---|---|
| 136 B | encode | 1540.7 (0.69x) | 2226.3 |
| 136 B | decode | 788.1 (1.22x) | 648.1 |
| 174 B | encode | 1483.3 (1.29x) | 1150.3 |
| 174 B | decode | 689.7 (1.22x) | 564.1 |
| 2469 B | encode | 1370.9 (1.59x) | 864.4 |
| 2469 B | decode | 500.8 (1.09x) | 457.6 |
| 74913 B | encode | 1413.5 (1.56x) | 906.3 |
| 74913 B | decode | 530.7 (1.16x) | 455.6 |

`pbwire-java` wins from 174 B up, and wins every decode tier: 1.2x at 174 B,
1.2-1.6x at 2469 B, 1.5-1.6x at 74913 B. The one tier it loses is the 136 B
encode (0.68x): `protobuf-java`'s generated writer is one straight-line block of
known tags and values (~62 ns), while a dynamic tree walk pays a presence test,
a type dispatch and a re-derived varint size per field (~90 ns including the
`byte[]` copy).

That gap is measured, not assumed: a prototype encoder walking the same value
tree with the wire type preloaded, no per-field capacity check and no `switch`
on the field type came in at 66.5 ns against 70.4 ns for `src/encode.c` on the
same 136-byte payload. The C side is within ~6% of its floor, so the remaining
~25 ns is the boundary plus the ~10 ns generated code saves by knowing its tags
at compile time -- closing it needs a code generator, not tuning.

## Layout

| Piece | Path | Role |
|---|---|---|
| FFI | [`runtime/Pbwire.java`](runtime/Pbwire.java) | one `MethodHandle` per exported C function, the per-thread workspace, library lookup, `pbw_status` → exception |
| API | [`runtime/Schema.java`](runtime/Schema.java), [`runtime/MessageType.java`](runtime/MessageType.java), [`runtime/Message.java`](runtime/Message.java), [`runtime/PbwireException.java`](runtime/PbwireException.java) | the wrapper and its field cache |
| example | [`examples/Roundtrip.java`](examples/Roundtrip.java) | build → encode → decode → compare |
| tests | [`tests/TestPbwire.java`](tests/TestPbwire.java) | assert-based self-check, no framework (includes a 4-thread schema-sharing check) |
| fixture | [`tests/data/sample.proto`](tests/data/sample.proto) + `sample.desc` | `protoc --descriptor_set_out --include_imports` |
| benchmark | [`benchmarks/Bench.java`](benchmarks/Bench.java) | the four standard tiers against `protobuf-java` |

The fixture descriptor set is checked in; regenerate it (protoc on `PATH`)
with:

```shell
protoc --descriptor_set_out=tests/data/sample.desc --include_imports \
       -I tests/data tests/data/sample.proto
```

## Deliberately skipped

- **No protoc plugin and no code generation.** The schema is the descriptor set
  loaded at runtime, like the C, C++ and Python bindings; a Java generator
  would duplicate what the C core already resolves.
- **No conformance testee.** The C core passes the official suite
  (`make conformance`); a testee here would only re-test the wrapper.
- **No Maven/Gradle**, no JNI, no C glue file, no downloads: `build.sh` is
  `make shared` plus `javac`. Classes are in the default package for the same
  reason (plain `javac -d out`, no module path); move them into a package if
  this ever ships as a jar.
- **No `Linker.Option.critical` downcalls.** They would let the wire calls read
  a heap `byte[]` and write one directly, but they forbid allocation and
  blocking, and `pbw_decode`/`pbw_encode` legitimately allocate; the one copy
  they would save is ~5 ns of a 90 ns op.
- **No map/oneof/extension convenience API.** They are ordinary message and
  field operations through the dynamic tree; nothing is lost, nothing is
  sugar-coated.
- **No lazy parsed-message representation** (the Python binding's trick of
  keeping the source bytes and materializing fields on read). Here `decode`
  builds the C message tree and `encode` re-encodes it; the byte-exact round
  trip comes from the tree keeping unknown fields in wire order.
- **No 32-bit host support in the FFI signatures**: `size_t` is mapped to a
  64-bit value layout (LP64: macOS and Linux on x86-64/arm64).
- **No release of the per-thread workspace.** A `pbw_ctx` has to be freed
  explicitly and a dead `Thread` gives no callback, so a thread that used the
  binding keeps one context (bounded by the largest message it handled) until
  the process exits. One context per thread is the same trade the core's own
  `pbw_pool` documents.

## Typed models

`protoc-gen-pbwire-typed` (in `../typed`) generates one Java class per message:
typed public fields plus `encode(Schema)` / `static decode(Schema, byte[])`.
The committed output is `tests/gen/`; `examples/Roundtrip.java` and
`tests/TestTyped.java` use only typed assignments:

```java
Tutorial_Person person = new Tutorial_Person();
person.name = "Alice";
person.id = 1;
byte[] wire = person.encode(schema);
```

See `../typed/README.md` for regeneration (`make typed-gen`) and tests
(`make typed-test`).
