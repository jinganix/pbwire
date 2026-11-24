# pbwire Kotlin binding

`bindings/kotlin/runtime` is the Kotlin binding. Kotlin runs on the JVM, so it
uses the Java binding's pure-Java codec (`bindings/java/runtime`, no JNI and no
native library) and adds the DSL that official Kotlin support also is:
`protoc --kotlin_out` generates Kotlin over protobuf-java, never a second
runtime. There is no new wire code and no second schema path -- a
`FileDescriptorSet` is loaded at run time and the Java codec owns the bytes.

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

`build.sh` prints the run commands (plain `java`/`kotlin`, no native library
path and no `--enable-native-access`, exactly like the Java binding). Kotlin
cannot import from Java's default package, which is why the runtime file declares
none.

## Benchmarks

```shell
./benchmarks/bench.sh    # vs protoc --kotlin_out over protobuf-java, four tiers
```

Downloads `protobuf-java` and `protobuf-kotlin` from Maven Central on first run.
The four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000).

#### Kotlin vs `protoc --kotlin_out` over `protobuf-java`

![Kotlin vs protoc --kotlin_out over protobuf-java — MB/s by tier](../../docs/charts/bench-kotlin-vs-protoc-kotlin-out-over-protobuf-java.svg)

| payload | op | pbwire-kotlin-core (MB/s, x protobuf-kotlin) | protobuf-kotlin (MB/s) |
|---|---|---|---|
| 136 B | encode | 533.1 (0.39x ❌) | 1367.1 |
| 136 B | decode | 327.3 (0.51x ❌) | 636.7 |
| 174 B | encode | 507.6 (0.58x ❌) | 878.7 |
| 174 B | decode | 306.3 (0.55x ❌) | 561.5 |
| 2469 B | encode | 447.8 (2.02x ✅) | 222.0 |
| 2469 B | decode | 291.5 (0.63x ❌) | 464.8 |
| 74913 B | encode | 463.0 (27.24x ✅) | 17.0 |
| 74913 B | decode | 324.0 (0.72x ❌) | 447.8 |

Kotlin reuses the Java runtime (`../java/runtime`), so these numbers are
`pbwire-java`'s: one workspace lookup plus one `byte[]` copy per call. `core`
is the same committed generated Java model the Kotlin DSL builds (the
`tests/gen/matrix3.kt` factories are inline sugar over it) and the published
comparison here; it tracks Java's `core` row (0.36x at 136 B, 1.10x at 74 KB
encode, 0.51x-0.80x decode) for the same reason: a generated writer knows
every tag at compile time while the dynamic tree walk derives them, the same
structural floor the Java section explains. The xlarge encode ratio (27.24x)
rides a `protobuf-kotlin` baseline collapse (17.0 MB/s there); decode is the
honest comparison. The `core` column is eager. The
per-field `tree` arm is compared in `../java/pbench/` (see its README),
not here. The `Message`-wrapper cut (no per-decode
`AtomicBoolean`, no volatile open flag) removes the binding's own per-call
overhead on top, so the narrow tiers track the codec instead of the wrapper.
Formal run
`../../results/bench-kotlin-20261124T094919Z.json`.

## Why not a copy of the Java runtime?

A second copy of the codec tables would be a second thing to keep in sync for
zero benefit: Kotlin calls Java classes with no
marshalling cost, and the JVM is the same host either way. The ownership and
threading rules are the Java binding's: a `Schema` or `Message` belongs to one
thread, `use`/`close` frees a root, and a nested message is a view of its parent.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) generates Kotlin `data class`es with
typed properties, `fun encode(schema)` and a `companion object { fun decode }`.
The committed output is `tests/gen/`; `examples/Roundtrip.kt` uses only typed
assignments. See `../../codegen/README.md`.

