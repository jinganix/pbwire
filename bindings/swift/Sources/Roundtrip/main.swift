// Runnable round trip through the Swift binding's dynamic API: build,
// encode, decode, assert, print. Run it with `swift run Roundtrip` (or
// `./build.sh example`) from bindings/swift.
import Foundation
import Pbwire

private func check(_ condition: Bool, _ label: String) {
    guard condition else {
        fputs("roundtrip: FAILED: \(label)\n", stderr)
        exit(1)
    }
}

// #filePath is the compiled source path, so the fixture resolves without
// depending on the working directory.
let sourceDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let fixture = sourceDir
    .deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("tests/data/probe.desc").path

let schema = try Schema(contentsOfFile: fixture)
let message = schema.message("pbwswift.Probe")!

try message.set(.int(1234), "i32")
try message.set(.string("alice"), "text")
try message.makeNested("inner").set(.string("child"), "label")

let wire = try message.encode() // proto3 bytes
let decoded = try message.decode(wire)

check(decoded.get("text") == .string("alice"), "text round trip")
check(decoded.nested("inner")?.get("label") == .string("child"), "nested round trip")
check(try decoded.encode() == wire, "byte-exact re-encode")

print("roundtrip: i32=\(decoded.get("i32")!) text=\(decoded.get("text")!) wire=\(wire.count) bytes")
print("roundtrip: all checks passed")
