// swift-tools-version: 5.9
import PackageDescription

// The static archive is the C core built at the repo root (`make`); the
// relative path resolves against this directory, so plain `swift build`,
// `swift test` and `swift run` work with no environment setup. A package with
// unsafe flags cannot itself be a dependency, which is fine: this binding is
// an end-user FFI host, like the Objective-C and Dart bindings.
let pbwireLink = LinkerSetting.unsafeFlags(["../../build/libpbwire.a"])

let package = Package(
    name: "pbwire",
    products: [
        .library(name: "Pbwire", targets: ["Pbwire"]),
        // The pure-Swift core: no C module, no `libpbwire.a`, no unsafeFlags,
        // so this product can be a normal SwiftPM dependency. The FFI product
        // above stays the default (see pbench/README.md#verdict).
        .library(name: "PbwireCore", targets: ["PbwireCore"]),
        // The official conformance testee (bindings/swift/conformance/): the
        // 4-byte-framed request/response protocol over the pure core, so
        // `make conformance-swift` drives this port, not the C core. An
        // executable product adds nothing to a library consumer's build.
        .executable(name: "pbwire-conformance", targets: ["PbwireConformance"]),
    ],
    targets: [
        // Re-exports the C core header (repo include/pbwire.h) as module
        // CPbwire, and compiles the C binding's layout codec
        // (bindings/c/runtime/pbw_struct.c, symlinked here) into this target
        // so the Swift `shim` arm reaches pbw_struct_codec_* in one call. No
        // wire code of its own: the format stays in the C core.
        .target(
            name: "CPbwire",
            path: "Sources/CPbwire",
            publicHeadersPath: "include"),
        // The Swift wrapper: Schema, Message, PbwireValue, PbwireError.
        .target(
            name: "Pbwire",
            dependencies: ["CPbwire"],
            path: "Sources/Pbwire"),
        // The pure-Swift wire core: descriptor reader, value tree, wire
        // primitives, encode/decode. Deliberately depends on nothing.
        .target(
            name: "PbwireCore",
            path: "Sources/PbwireCore"),
        // The conformance testee: the official testee protocol (length-prefixed
        // ConformanceRequest/Response) on stdin/stdout over the pure core.
        .executableTarget(
            name: "PbwireConformance",
            dependencies: ["PbwireCore"],
            path: "conformance",
            exclude: ["README.md"]),
        // Generated addressbook models over the Pbwire runtime (codegen:
        // `make -C codegen gen-examples`). No .desc file at use time.
        .target(
            name: "AddressbookGen",
            dependencies: ["Pbwire"],
            path: "examples/gen"),
        // Runnable round trip through the dynamic API.
        .executableTarget(
            name: "Roundtrip",
            dependencies: ["Pbwire", "AddressbookGen"],
            path: "Sources/Roundtrip",
            linkerSettings: [pbwireLink]),
        // pbench arms: the tree and shim timing loops and their payload on
        // matrix3 (the generated mirror is symlinked in from the test model).
        .target(
            name: "Pbench",
            dependencies: ["Pbwire", "PbwireCore"],
            path: "Sources/Pbench"),
        // The four standard tiers (see BENCHMARKS.md) on matrix3.desc.
        .executableTarget(
            name: "Bench",
            dependencies: ["Pbwire", "Pbench"],
            path: "Sources/Bench",
            linkerSettings: [pbwireLink]),
        // Assert-based self-check, no dependencies beyond XCTest.
        .testTarget(
            name: "PbwireTests",
            dependencies: ["Pbwire"],
            path: "Tests/PbwireTests",
            linkerSettings: [pbwireLink]),
        // The pure core's own gate: descriptor, codec, wire. No C dependency,
        // so this suite builds and runs with the C archive absent.
        .testTarget(
            name: "PbwireCoreTests",
            dependencies: ["PbwireCore"],
            path: "Tests/PbwireCoreTests"),
    ]
)
