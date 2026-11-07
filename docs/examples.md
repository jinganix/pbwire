# Examples index

One runnable round trip per binding: build the tutorial message, encode it,
decode it back, print it. Most run from the repo root; every command below
is quoted from that binding's README or `build.sh`.

| binding | example | run |
|---|---|---|
| C | [`bindings/c/examples/addressbook.c`](../bindings/c/examples/addressbook.c), [`gateway.c`](../bindings/c/examples/gateway.c) | `make test` builds and runs both |
| C++ | [`bindings/cpp/examples/addressbook.cc`](../bindings/cpp/examples/addressbook.cc) | `make examples` |
| Embedded | [`bindings/embedded/examples/roundtrip.c`](../bindings/embedded/examples/roundtrip.c) | `make test` builds and runs it |
| Python | [`bindings/python/examples/addressbook.py`](../bindings/python/examples/addressbook.py) | `python bindings/python/examples/addressbook.py` |
| Go | [`bindings/go/examples/roundtrip.go`](../bindings/go/examples/roundtrip.go) | `go run ./examples/roundtrip.go` (from `bindings/go`) |
| Java | [`bindings/java/examples/Roundtrip.java`](../bindings/java/examples/Roundtrip.java) | `cd bindings/java && ./build.sh` (self-check + example) |
| Kotlin | [`bindings/kotlin/examples/Roundtrip.kt`](../bindings/kotlin/examples/Roundtrip.kt) | `cd bindings/kotlin && ./build.sh` (fat jar + `RoundtripKt`) |
| Clojure | [`bindings/clojure/examples/roundtrip.clj`](../bindings/clojure/examples/roundtrip.clj) | `cd bindings/clojure && ./build.sh` (`clojure.main -m roundtrip`) |
| Scala | [`bindings/scala/examples/`](../bindings/scala/examples/) | `cd bindings/scala && ./build.sh` (javac + scalac, self-check + example) |
| Node | [`bindings/node/examples/roundtrip.js`](../bindings/node/examples/roundtrip.js) | `node bindings/node/examples/roundtrip.js` |
| Ruby | [`bindings/ruby/examples/roundtrip.rb`](../bindings/ruby/examples/roundtrip.rb) | `ruby -Ibindings/ruby/lib bindings/ruby/examples/roundtrip.rb` |
| Objective-C | [`bindings/objc/examples/`](../bindings/objc/examples/) | `bindings/objc/build.sh example` |
| PHP | [`bindings/php/examples/roundtrip.php`](../bindings/php/examples/roundtrip.php) | `php examples/roundtrip.php` (from `bindings/php`) |
| Dart | [`bindings/dart/examples/roundtrip.dart`](../bindings/dart/examples/roundtrip.dart) | `dart run examples/roundtrip.dart` (from `bindings/dart`) |
| Rust | [`bindings/rust/examples/roundtrip.rs`](../bindings/rust/examples/roundtrip.rs) | `cargo run --example roundtrip -- ../c/tests/data/matrix3.desc` (from `bindings/rust`) |
| C# | [`bindings/csharp/examples/`](../bindings/csharp/examples/) | `dotnet run --project examples -- ../c/tests/data/matrix3.desc` (from `bindings/csharp`) |
| F# | [`bindings/fsharp/examples/`](../bindings/fsharp/examples/) | `dotnet run --project examples -- ../c/tests/data/matrix3.desc` (from `bindings/fsharp`) |
| Lua | [`bindings/lua/examples/roundtrip.lua`](../bindings/lua/examples/roundtrip.lua) | `lua examples/roundtrip.lua` (from `bindings/lua`; also run by `make lua-test`) |
| Erlang | [`bindings/erlang/examples/addressbook.escript`](../bindings/erlang/examples/addressbook.escript) | `escript examples/addressbook.escript` (from `bindings/erlang`) |
| Elixir | [`bindings/elixir/examples/addressbook.exs`](../bindings/elixir/examples/addressbook.exs) | `mix run examples/addressbook.exs` (from `bindings/elixir`) |
| Zig | [`bindings/zig/examples/roundtrip.zig`](../bindings/zig/examples/roundtrip.zig) | `zig build example` (from `bindings/zig`) |
| Swift | [`bindings/swift/examples/gen/`](../bindings/swift/examples/gen/) (generated model) | `cd bindings/swift && ./build.sh example` |
| Julia | [`bindings/julia/examples/roundtrip.jl`](../bindings/julia/examples/roundtrip.jl) | `julia --project=runtime examples/roundtrip.jl` (from `bindings/julia`) |
| R | [`bindings/r/examples/roundtrip.R`](../bindings/r/examples/roundtrip.R) | `Rscript examples/roundtrip.R` (from `bindings/r`) |

Typed variants of the same round trip live in `codegen`: every binding's
`tests/gen/` (committed output of `protoc-gen-pbwire`) and `examples/gen/`
(the same addressbook through the generated model). The official-conformance
angle is in [`testing.md`](testing.md#5-bindings); the numbers angle in
[`../BENCHMARKS.md`](../BENCHMARKS.md).
