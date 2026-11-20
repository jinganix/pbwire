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
            dependencies: ["Pbwire"],
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
    ]
)
