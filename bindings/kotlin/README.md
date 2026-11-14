# pbwire Kotlin binding

`bindings/kotlin/runtime` is the Kotlin binding. Kotlin runs on the JVM, so it
uses the Java binding's FFI layer (`bindings/java/runtime`: the JDK 22+ Foreign
Function & Memory downcalls to the C ABI in
[`include/pbwire.h`](../../include/pbwire.h)) and adds the DSL that official
Kotlin support also is: `protoc --kotlin_out` generates Kotlin over
protobuf-java, never a second runtime. There is no new wire code and no second
schema path -- a `FileDescriptorSet` is loaded at run time and the C core owns
the bytes.

```kotlin
schema("tests/data/matrix3.desc").use { schema ->
    val ty = schema.message("structtest.Scalars3")

    ty.build {
        set("i32", -42L)
        set("str", "alice")
        for (tag in listOf("a", "bb")) {
            set("tags", tag)                 // repeated: appends
        }
        message("inner") {                   // nested: created in place
            set("name", "kid")
            set("value", 42L)
        }
    }.use { msg ->
        val wire = msg.encode()
        ty.parse(wire).use { decoded ->
            assert(decoded.getLong("i32") == -42L)
            assert(decoded.getList("tags") == listOf("a", "bb"))
            assert(decoded.encode().contentEquals(wire))   // byte-exact round trip
        }
    }
}
```

The runtime is four helpers (`schema`, `build`, `parse`, `message`); everything
else is the Java binding's API, which Kotlin calls directly.

## Build and run

```shell
./build.sh          # make shared + javac the Java runtime + kotlinc into out/pbwire.jar
```

`build.sh` prints the two run commands (they need `-Dpbwire.library=<path>` and
`--enable-native-access=ALL-UNNAMED`, exactly like the Java binding). Kotlin
cannot import from Java's default package, which is why the runtime file declares
none.

## Benchmarks

```shell
./benchmarks/bench.sh    # vs protoc --kotlin_out over protobuf-java, four tiers
```

Downloads `protobuf-java` and `protobuf-kotlin` from Maven Central on first run.
The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Copied from [BENCHMARKS.md](../../BENCHMARKS.md),
which is the single source for all performance numbers; higher is better, MB/s
over wire bytes:

| payload | op | pbwire-kotlin (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 1729.7 (0.8x) | 2109.9 |
| 136 B | decode | 934.8 (1.5x) | 634.2 |
| 174 B | encode | 627.8 (0.5x) | 1189.7 |
| 174 B | decode | 777.0 (1.4x) | 560.5 |
| 2469 B | encode | 967.3 (1.1x) | 861.4 |
| 2469 B | decode | 772.7 (1.6x) | 470.2 |
| 74913 B | encode | 934.3 (1.1x) | 833.7 |
| 74913 B | decode | 788.1 (1.7x) | 453.1 |

## Why not a copy of the Java runtime?

A second copy of the FFM tables would be a second thing to keep in sync with
`include/pbwire.h` for zero benefit: Kotlin calls Java classes with no
marshalling cost, and the JVM is the same host either way. The ownership and
threading rules are the Java binding's: a `Schema` or `Message` belongs to one
thread, `use`/`close` frees a root, and a nested message is a view of its parent.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates Kotlin `data class`es with
typed properties, `fun encode(schema)` and a `companion object { fun decode }`.
The committed output is `tests/gen/`; `examples/Roundtrip.kt` uses only typed
assignments. See `../../codegen/README.md`.
