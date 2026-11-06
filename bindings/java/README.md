# pbwire Java binding

`bindings/java/runtime` is the Java binding: a pure-Java codec with no
JNI, no `java.lang.foreign` and no native library. Two APIs share the wire
primitives in `Wire`: the dynamic `Message` tree over a runtime schema (a
`FileDescriptorSet` via `protoc --descriptor_set_out`, parsed by `Proto`
and owned by `GenericCodec`), and typed POJOs with two annotations
(`PbCodec.encode`/`parseFrom` straight into a `Wire` buffer, no tree, no
schema file). The POJO is your own class: Lombok and bean-validation
annotations ride along untouched because the codec reads fields directly
and looks only at `@PbMessage`/`@PbField`.

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
./build.sh          # plain javac into bindings/java/out

java -cp out TestPbwire tests/data/sample.desc     # self-check
java -cp out Roundtrip tests/data/sample.desc      # example
```

`build.sh` is plain `javac` (JDK 17+): no Maven, no Gradle, no native
library, no `--enable-native-access`. The fixture descriptor set is checked
in, so a build needs no `protoc` either.

## API

| class | members |
|---|---|
| `PbCodec` | `encode(pojo)`, `parseFrom(bytes, MyClass.class)`, `isInitialized(pojo)` — any `@PbField` class, no registration |
| `PbMessage` / `PbField` / `PbKind` | the two annotations plus the kind enum (`AUTO` infers from the Java type; set it for `UINT32`/`SINT32`/`FIXED32`/`ENUM`/`GROUP` and friends) |
| `Schema` | `fromFile(path)`, `fromBytes(byte[])`, `fromClasses(Class...)`, `message(fullName)`, `close()` |
| `MessageType` | `name()`, `newMessage()`, `decode(byte[])` |
| `Message` | `set(name\|number, …)`, `newMessage(name\|number)`, `clear(…)`, `has(…)`, `get(…)`, typed getters, `getList(…)`, `encode()`, `close()` |
| `PbwireException` | unchecked; `decode`/`encode` failures and unknown fields/types name the message and field |

`set` overloads: `long` (every integer, fixed and enum field, raw 64-bit bits),
`boolean`, `double` (a `float` field is narrowed for you), `String` (UTF-8),
`byte[]`. `set` replaces a singular field and appends to a repeated one; there
is no separate `add`. `newMessage` returns a child `Message` that writes
straight into the tree (append for a repeated message field).

`fromClasses` builds the same schema out of generated message classes instead
of a descriptor file: the codegen tables carry every field fact the dynamic
codec needs (names, numbers, kinds, packed/oneof), so generated and dynamic
code read the same bytes. Pass only the roots; sub-messages follow the table
links. A descriptor schema and a `fromClasses` schema coexist in one process
— they share nothing but the wire format.

Getters: `get(name\|number)` returns the boxed value and `null` when absent;
`getLong/getInt/getBool/getDouble/getFloat/getString/getBytes/getMessage`
return the Java default (`0`, `false`, `null`) when absent; `getList` returns an
empty list. `has` is the presence test — a field that was not set (or not on the
wire) is absent, matching pbwire's explicit-presence model. Enums are read as
integers. A `uint64` above `Long.MAX_VALUE` reads as the same raw bits, i.e. a
negative `long`.

Value mapping: `int32/int64/sint*/sfixed*/fixed*` → `long` (signed),
`uint32`/`fixed32` → `long` (zero-extended, so 4000000000 reads as
4000000000L), `uint64`/`fixed64` → `long` (raw bits, negative above
`Long.MAX_VALUE`), `enum` → `long`/`int`, `bool` → `boolean`, `float/double` → `double` (or `Float`/`Double`
from `get`), `string` → `String`, `bytes` → `byte[]`, `message` → `Message`,
`repeated` → `List<Object>` (or `List<Message>` elements).

Errors are unchecked: `PbwireException` for descriptor problems, malformed
wire bytes and a proto2 `required` field missing on encode;
`IllegalArgumentException` for a wrong value type or unknown field number;
`IllegalStateException` for using a closed schema/message; `UncheckedIOException`
for a missing descriptor file.

Ownership is explicit and race-free:

- `Schema.close()` marks the schema unusable; `Message.close()` retires the
  tree created by `newMessage()`/`decode()` and is idempotent. Both throw
  `IllegalStateException` once closed.
- A child `Message` from `newMessage()`/`getMessage()` is a view into the
  same tree: it is never freed on its own, and it shares the root's closed
  flag, so using it after the root is closed throws.
- A decoded tree is one object graph released with its root; nothing is
  shared between threads except the schema.

## How the codec stays fast

One workspace per thread holds everything an encode needs: the reusable
output buffer (so `encode()` allocates nothing but the returned `byte[]`)
and the string-image cache. Field facts (kind, wire type, tag bytes,
presence class) are resolved once per message type, so the encode loop is a
single pass: one presence gate per slot, one `switch` per numeric field,
fused tag+value writes with one capacity check each. Strings re-emit as one
`arraycopy` of a cached whole-field image. Numbers, fixed widths and tags go
through `sun.misc.Unsafe` stores with no per-byte bounds checks -- the same
trick the official encoder uses -- with a checked fallback when `Unsafe` is
missing or the platform is big-endian.

On top of that, decode is lazy: message, group, string and bytes fields
record a slice (input ref + bounds) and only scan tags, so untouched
sub-trees cost nothing until first access and re-encode copies them verbatim
(which is also what keeps unknown fields and byte-exactness for free).
That makes `Schema`/`MessageType` safe to share between threads -- a schema is
read-only after load -- while a `Message` and the workspace stay confined to
one thread (see `TestPbwire`'s 4-thread check).

## Benchmark

`benchmarks/bench.sh` generates `protobuf-java` from the same `matrix3.proto`
with `--java_out` and runs the four standard tiers from
[`BENCHMARKS.md`](../../BENCHMARKS.md), printing `ns/op` and `MB/s` for
`pbwire-java` and `protobuf-java` side by side. It fetches `protobuf-java` from
Maven Central once (into `benchmarks/deps/`) and needs JDK 22+ and `protoc`.
All four operations get a warm-up pass, then three interleaved timed passes whose
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
| 136 B | encode | 1831.6 (0.8x) | 2243.7 |
| 136 B | decode | 939.2 (1.5x) | 633.9 |
| 174 B | encode | 1575.1 (1.2x) | 1264.2 |
| 174 B | decode | 845.7 (1.5x) | 550.5 |
| 2469 B | encode | 1023.6 (1.2x) | 873.3 |
| 2469 B | decode | 892.9 (2.0x) | 446.3 |
| 74913 B | encode | 1061.4 (1.2x) | 904.7 |
| 74913 B | decode | 968.7 (2.4x) | 411.0 |

`pbwire-java` wins from 174 B up on encode and wins every decode tier: 1.2x
at 174 B, 1.2x at 2469 B, 1.2x at 74913 B on encode; 1.5x, 2.0x and 2.4x on
decode. The one cell it loses is the 136 B encode (0.82x): `protobuf-java`'s
generated writer is one straight-line block of known tags and values (~60 ns),
while a dynamic codec pays a presence gate, a type dispatch and tag loads per
field (~80 ns including the `byte[]` copy). That floor is structural, not
tuning: the per-field work left -- loads, one `switch`, one call -- is what a
generated writer avoids by knowing every field at compile time. The typed
POJOs (`codegen`) make the mirror trade: they skip the tree and write
straight into the wire buffer, but reflection and boxed lists cost
(1.7x the dynamic encode time, ~1.0x the decode time on the ~33 KB tier).
Keep hot loops on the dynamic `Message` path; keep domain code on POJOs.

## Layout

| Piece | Path | Role |
|---|---|---|
| codec | [`runtime/GenericCodec.java`](runtime/GenericCodec.java) | columnar value tree, lazy decode slices, the single-pass encoder (the fast path) |
| typed | [`runtime/PbCodec.java`](runtime/PbCodec.java), [`runtime/PbMessage.java`](runtime/PbMessage.java), [`runtime/PbField.java`](runtime/PbField.java), [`runtime/PbKind.java`](runtime/PbKind.java) | reflective POJO table (cached per class): fields in number order, direct `Wire` writes, no tree |
| wire | [`runtime/Wire.java`](runtime/Wire.java) | varint/tag/little-endian read/write, reusable buffer, `Unsafe` fast path |
| schema | [`runtime/Proto.java`](runtime/Proto.java), [`runtime/Schema.java`](runtime/Schema.java) | descriptor-set parser and linker, no protobuf dependency |
| API | [`runtime/MessageType.java`](runtime/MessageType.java), [`runtime/Message.java`](runtime/Message.java), [`runtime/PbwireException.java`](runtime/PbwireException.java) | the dynamic wrapper: typed getters, views, presence, errors |
| example | [`examples/Roundtrip.java`](examples/Roundtrip.java) | build → encode → decode → compare |
| conformance | [`conformance/Testee.java`](conformance/Testee.java) | official suite testee (binary in → out, JSON/text skip) |
| tests | [`tests/TestPbwire.java`](tests/TestPbwire.java), [`tests/TestGenerated.java`](tests/TestGenerated.java), [`tests/TestCustom.java`](tests/TestCustom.java) | assert-based self-checks, no framework: dynamic gate, generated-POJO compat (T1–T6), hand-written Lombok/validation shapes |
| fixture | [`tests/data/sample.proto`](tests/data/sample.proto) + `sample.desc` | `protoc --descriptor_set_out --include_imports` |
| benchmark | [`benchmarks/Bench.java`](benchmarks/Bench.java) | the four standard tiers against `protobuf-java` |

The fixture descriptor set is checked in; regenerate it (protoc on `PATH`)
with:

```shell
protoc --descriptor_set_out=tests/data/sample.desc --include_imports \
       -I tests/data tests/data/sample.proto
```

## Deliberately skipped

- **No second wire format in codegen.** The plugin emits annotated POJOs
  only; every byte still comes from the `Wire` primitives the dynamic path
  uses, so the two APIs cannot diverge.
- **Binary-only conformance testee.** `make conformance-java` runs the official
  suite through the binding (same 2 expected tag-truncation failures as the C
  core, on the shared `conformance/failure_list_pbwire.txt`); JSON/text input
  answers `skipped` because the binding has no printers, like the Python and
  Ruby testees.
- **No Maven/Gradle**, no JNI, no C glue file, no downloads: `build.sh` is
  plain `javac`. Classes are in the default package for the same
  reason (plain `javac -d out`, no module path); move them into a package if
  this ever ships as a jar.
- **No per-call bounds checks on the encode hot path.** Tags, varints and
  fixed widths go through `sun.misc.Unsafe` (the same trick the official
  encoder uses); without `Unsafe`, or off little-endian, the same methods
  run their checked fallback. The one runtime cost is a terminal-deprecation
  warning printed once per JVM, like `protobuf-java` itself prints.
- **No lazy string copies.** A decoded string materializes on first read;
  re-encoding a decoded message copies untouched slices verbatim, so a
  gateway that only re-encodes never pays for parsing.
- **No extension convenience API.** Extensions are ordinary fields through
  the dynamic tree; nothing is lost, nothing is sugar-coated. Maps and
  oneofs are first-class in the typed POJOs (`Map` fields, nullable oneof
  members with `getKindCase()`).
- **No per-thread workspace release.** The encode buffer and the string-image
  cache live in one `ThreadLocal` per thread and are never freed: a thread
  that used the binding keeps one buffer (bounded by the largest message it
handled) until the thread dies. Keep request threads off the binding or reuse
  a fixed pool.

## Typed POJOs: your own message classes

`protoc-gen-pbwire` (in `../../codegen`) generates one annotated POJO per
message: public fields with `@PbField` numbers, `Map` for maps, boxed
nullables for explicit presence and oneof members, `toByteArray` straight
into a `Wire.Buf` and `parseFrom` straight from a `Wire.Reader` — no
`Message` tree, no per-field lookup, no schema file at use time. The
committed output is `tests/gen/`; `examples/Roundtrip.java` fills fields
directly:

```java
Tutorial_Person person = new Tutorial_Person();
person.name = "Alice";
person.id = 1;
byte[] wire = person.toByteArray();
```

The generated class is a starting point, not a cage: delete it and write
your own. Any class with `@PbField` numbers encodes with zero setup, so the
Lombok and bean-validation stack you already use rides along — the codec
reads fields directly and ignores every annotation except the two below:

```java
@PbMessage("tutorial.Person")
@Data @NoArgsConstructor @AllArgsConstructor
public class Person {
    @PbField(value = 1, required = true) @NotBlank String name;
    @PbField(value = 2, required = true) int id;
    @PbField(value = 3, optional = true) @Email String email;
    @PbField(value = 4) List<Phone> phones = new ArrayList<>();
}

byte[] wire = PbCodec.encode(person);
Person back = PbCodec.parseFrom(wire, Person.class);
```

Presence follows the Java type: primitives are implicit (zero stays off the
wire), boxed numerics, `Optional`, messages and `optional=true`
strings/bytes are explicit (a set zero/empty stays on the wire), lists and
maps are empty-absent. Oneof members share `oneof="..."` and must be
nullable — setting two fails encode loudly, decode keeps the last one on
the wire. Enums ride as `int` (raw wire values, unknowns preserved);
`Map<K, V>` rides as a real map. Records work through their canonical
constructor; anything else needs a no-arg constructor (Lombok
`@NoArgsConstructor` supplies it). `tests/TestCustom.java` proves each
shape with hand-written Lombok/validation stand-ins.

See `../../codegen/README.md` for regeneration (`make codegen`) and tests
(`make codegen-test`).
