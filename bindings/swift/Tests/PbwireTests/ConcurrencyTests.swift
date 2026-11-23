// Concurrency check for the Swift binding: 8 workers x 200 iters over the
// matrix3 oracle, mirroring the C/C++/Rust/Zig shape:
//
//   (a) per-iteration message encode == want bytes,
//   (b) SHARED read-only message concurrent encode == want bytes,
//   (c) fresh-target decode of want with field checks (NOT re-encode).
//
// The embedded schema (tests/gen/matrix3.swift) and the want bytes are the
// only cross-thread state, both read-only after startup. Every worker builds
// per-iteration local messages and every encode returns its own Data, so no
// buffer is ever shared across threads. Speed prints only, never asserted.
//
// Run it with `swift test --filter ConcurrencyTests` from bindings/swift.
import Dispatch
import Foundation
import XCTest

final class ConcurrencyTests: XCTestCase {
    static var bin: String {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tests/data/matrix3.bin").path
    }

    // The populated matrix3.txt values, same as GeneratedTest: the oracle
    // bytes only match this exact content.
    private func build() -> Structtest_Scalars3 {
        var inner = Structtest_Inner3()
        inner.name = "kid"
        inner.value = 42
        var x = Structtest_Inner3()
        x.name = "x"
        x.value = 1
        var y = Structtest_Inner3()
        y.name = "y"
        y.value = -2
        var m = Structtest_Scalars3()
        m.i32 = -2147483648
        m.i64 = 9223372036854775807
        m.u32 = 4294967295
        m.u64 = 18446744073709551615
        m.s32 = -2147483648
        m.s64 = -9223372036854775808
        m.fx32 = 3735928559
        m.fx64 = 72623859790382856
        m.sfx32 = -2147483648
        m.sfx64 = -9223372036854775808
        m.f32 = Float.infinity
        m.f64 = -123456.789
        m.flag = true
        m.str = "hello"
        m.color = 2
        m.inner = inner
        m.nums = [1, -5, 300]
        m.tags = ["a", "bb"]
        m.inners = [x, y]
        m.optI32 = -7
        m.optStr = "opt"
        m.flags = [true, false, true]
        return m
    }

    func testConcurrentEncode() throws {
        let want = try Data(contentsOf: URL(fileURLWithPath: Self.bin))
        // Serial baseline: one encode must equal the protoc oracle before any
        // worker starts, so a mapping bug fails here and not in a thread.
        XCTAssertEqual(
            try build().encode(), want,
            "serial encode equals the protoc reference bytes")
        // Shared read-only fixture for phase (b): never mutated while
        // workers run; each encode builds a fresh tree and Data from it.
        let shared = build()

        let start = CFAbsoluteTimeGetCurrent()
        let lock = NSLock()
        var problems: [String] = []
        func record(_ worker: Int, _ phase: String) {
            lock.lock()
            problems.append("worker \(worker): \(phase)")
            lock.unlock()
        }
        DispatchQueue.concurrentPerform(iterations: 8) { worker in
            for _ in 0..<200 {
                // (a) independent-message encode == want bytes.
                do {
                    if try self.build().encode() != want {
                        record(worker, "phase (a) independent-encode != want bytes")
                    }
                } catch {
                    record(worker, "phase (a) independent-encode threw \(error)")
                }
                // (b) SHARED read-only message concurrent encode == want.
                do {
                    if try shared.encode() != want {
                        record(worker, "phase (b) shared-read-only-encode != want bytes")
                    }
                } catch {
                    record(worker, "phase (b) shared-read-only-encode threw \(error)")
                }
                // (c) fresh-target decode with field checks, NOT re-encode.
                do {
                    let back = try Structtest_Scalars3.decode(want)
                    guard back.i32 == -2147483648,
                        back.u64 == 18446744073709551615,
                        back.f32 == Float.infinity,
                        back.f64 == -123456.789,
                        back.flag == true,
                        back.str == "hello",
                        back.color == 2,
                        back.inner?.name == "kid",
                        back.inner?.value == 42,
                        back.nums == [1, -5, 300],
                        back.tags == ["a", "bb"],
                        back.inners.count == 2,
                        back.inners[1].value == -2,
                        back.optI32 == -7,
                        back.optStr == "opt",
                        back.flags == [true, false, true]
                    else {
                        record(worker, "phase (c) fresh-target decode field mismatch")
                        continue
                    }
                } catch {
                    record(worker, "phase (c) fresh-target decode threw \(error)")
                }
            }
        }
        let elapsed = CFAbsoluteTimeGetCurrent() - start
        let total = 8 * 200 * 3
        print(String(
            format: "concurrent: %d encodes+decodes in %.3f s (%.0f/s)",
            total, elapsed, Double(total) / elapsed))
        XCTAssertTrue(
            problems.isEmpty,
            problems.prefix(8).joined(separator: "; "))
    }
}
