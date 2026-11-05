# pbwire Scala binding

`bindings/scala/runtime` is the Scala binding. Scala runs on the JVM, so it
uses the Java binding's runtime (`bindings/java/runtime`: the pure-Java
codec, no JNI, no native library) and adds only the DSL that official Scala
support also is: ScalaPB generates case classes over protobuf-java, never a
second runtime. There is no new wire code and no second schema path -- a
`FileDescriptorSet` is loaded at run time and the Java runtime owns the
bytes.

```scala
import Pbwire._

using(schema("tests/data/matrix3.desc")) { schema =>
  val ty = schema.message("structtest.Scalars3")

  using(ty.build { m =>
    m.set("i32", -42L)
    m.set("str", "alice")
    List("a", "bb").foreach(v => m.set("tags", v)) // repeated: appends
    m.message("inner") { c =>                       // nested: created in place
      c.set("name", "kid")
      c.set("value", 42L)
    }
  }) { msg =>
    val wire = msg.encode()
    using(ty.parse(wire)) { decoded =>
      assert(decoded.getLong("i32") == -42L)
      assert(decoded.getList("tags").toString == "[a, bb]")
      assert(decoded.encode().sameElements(wire))   // byte-exact round trip
    }
  }
}
```

The runtime is one object (`Pbwire`): `schema` loads a descriptor set by
path or bytes, `using` is the loan pattern, and two implicit classes add
`build`/`parse` on `MessageType` and `message`/`use` on `Message`.
Everything else is the Java binding's API, which Scala calls directly.

## Build and run

```shell
./build.sh          # javac the Java runtime + scalac the DSL, self-check and example
```

`build.sh` needs JDK 17+ and `scalac` (Scala 2.13+ or 3.x): no sbt, no
Maven, no downloads. It prints the run commands. `build.sbt` is the same
project for sbt users: it compiles the Java runtime sources in place, so
the two runtimes stay in sync without a published artifact.

The DSL declares no package: the Java runtime lives in the default
package, which a named package cannot see. Same reason as the Kotlin
binding's runtime.

## Benchmarks

```shell
./benchmarks/bench.sh    # vs ScalaPB generated code, four tiers
```

Needs `scalac`, `protoc` and `scalapbc` (the ScalaPB compiler); downloads
the ScalaPB runtime and protobuf-java from Maven Central on first run. The
four tiers are small / medium / large / xlarge (the repeated fields scaled
x0 / x1 / x100 / x3000). Copied from [BENCHMARKS.md](../../BENCHMARKS.md),
which is the single source for all performance numbers; higher is better,
MB/s over wire bytes.

## Conformance

`conformance/Testee.scala` is the official-suite testee: binary in ->
binary out through the dynamic API, no per-message code. JSON/text input
answers `skipped` (the binding has no printers), like the Java and Python
testees. It shares the two expected binary tag-truncation failures on the
repo-wide `conformance/failure_list_pbwire.txt`.

## Why not a copy of the Java runtime?

A second copy of the codec would be a second thing to keep in sync with
the descriptor format for zero benefit: Scala calls Java classes with no
marshalling cost, and the JVM is the same host either way. The ownership
and threading rules are the Java binding's: a `Schema` or `MessageType`
may be shared between threads, a `Message` belongs to one thread,
`using`/`use`/`close` retires a root, and a nested message is a view of
its parent.

## Generated models

`protoc-gen-pbwire` (in `../../codegen`) emits the per-language message
model; the Scala shape is still to be wired up (the parent integrates
`codegen/`). Until then the committed `tests/` use the dynamic API only.
See `../../codegen/README.md`.

## Deliberately skipped

- **No second codec, no JNI, no native library.** The Java runtime is the
  wire format; this binding is one DSL object over it.
- **No protoc plugin and no code generation.** The schema is the
  descriptor set loaded at runtime, like the C, C++, Python and Java
  bindings.
- **No sbt/Maven downloads in the gate.** `build.sh` is `javac` +
  `scalac` against the checked-in fixture descriptor sets.
- **No map/oneof/extension convenience API.** They are ordinary message
  and field operations through the dynamic tree; nothing is lost, nothing
  is sugar-coated.
