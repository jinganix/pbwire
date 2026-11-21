// Zig binding build: compiles the C core into a static library and links it
// into every artifact, the same class as the Rust binding (no dlopen, no
// second wire implementation, single self-contained binary). The schema always
// comes from a FileDescriptorSet loaded at run time; nothing here parses
// `.proto` text.
//
//   zig build test     self-check (zig test over tests/selftest.zig)
//   zig build example  run the round-trip example
//   zig build bench    run the four standard tiers (needs a matrix3.desc path)
//   zig build testee   build the conformance testee binary
//
// Written against the Zig 0.17 module-based build API (everything hangs off
// b.createModule; Step.Compile takes only name + root_module).
const std = @import("std");

const core_sources = [_][]const u8{
    "arena.c",
    "ctx.c",
    "decode.c",
    "descriptor_set.c",
    "encode.c",
    "json.c",
    "message.c",
    "schema.c",
    "stream.c",
    "text.c",
    "wire.c",
    "wkt.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    // `-Dcore=true` selects the pure-Zig core for the test and testee steps:
    // no C source, no translate-c, no `libpbwire.a` in that artifact. The
    // default stays the static-link build.
    const want_core = b.option(bool, "core", "Use the pure-Zig core (no C sources) for tests and the conformance testee") orelse false;

    // Static core library every artifact links.
    const core_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    core_mod.addIncludePath(b.path("../../include"));
    core_mod.addCSourceFiles(.{
        .root = b.path("../../src"),
        .files = &core_sources,
        .flags = &.{ "-std=c99", "-Wall" },
    });
    // The C binding's layout codec: the generated extern-struct mirror is
    // walked in place by this file, so the binding links one implementation
    // and adds no wire code of its own.
    core_mod.addIncludePath(b.path("../c/runtime"));
    core_mod.addCSourceFiles(.{
        .root = b.path("../c/runtime"),
        .files = &.{"pbw_struct.c"},
        .flags = &.{ "-std=c99", "-Wall" },
    });
    const lib = b.addLibrary(.{ .name = "pbwire", .root_module = core_mod });

    // The binding module itself: `const pbwire = @import("pbwire");`
    // Its C symbols come from translate-c (@cImport is gone since 0.15).
    const tc = b.addTranslateC(.{
        .root_source_file = b.path("runtime/c_shim.h"),
        .target = target,
        .optimize = optimize,
    });
    tc.addIncludePath(b.path("../../include"));
    tc.addIncludePath(b.path("../c/runtime"));
    const pbwire_mod = b.createModule(.{
        .root_source_file = b.path("runtime/pbwire.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{
            .{ .name = "c", .module = tc.createModule() },
        },
    });
    pbwire_mod.addIncludePath(b.path("../../include"));
    pbwire_mod.addIncludePath(b.path("../c/runtime"));

    // The pure-Zig core: no C source, no translate-c, no libc dependency.
    const pure_mod = b.createModule(.{
        .root_source_file = b.path("core/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // The generated matrix3 model, for the shim row both bench harnesses time.
    const model_mod = b.createModule(.{
        .root_source_file = b.path("tests/gen/matrix3.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{.{ .name = "pbwire", .module = pbwire_mod }},
    });

    // zig build test
    const unit_mod = b.createModule(.{
        .root_source_file = b.path("tests/selftest.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{.{ .name = "pbwire", .module = pbwire_mod }},
    });
    const unit = b.addTest(.{ .name = "selftest", .root_module = unit_mod });
    unit.root_module.linkLibrary(lib);
    const run_unit = b.addRunArtifact(unit);
    const test_step = b.step("test", "Run the self-check (zig test)");
    test_step.dependOn(&run_unit.step);

    // The core's own tests (descriptor gate + codec). They link no C code.
    const core_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/core/root.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{.{ .name = "pbwire_core", .module = pure_mod }},
    });
    const core_test = b.addTest(.{ .name = "coretest", .root_module = core_test_mod });
    const run_core_test = b.addRunArtifact(core_test);
    test_step.dependOn(&run_core_test.step);

    // zig build generated-test: matrix3 oracle through the generated structs.
    const gen_mod = b.createModule(.{
        .root_source_file = b.path("tests/generated_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{.{ .name = "pbwire", .module = pbwire_mod }},
    });
    const gen_test = b.addTest(.{ .name = "generatedtest", .root_module = gen_mod });
    gen_test.root_module.linkLibrary(lib);
    const run_gen = b.addRunArtifact(gen_test);
    const gen_step = b.step("generated-test", "Run the generated-model check");
    gen_step.dependOn(&run_gen.step);

    // zig build example [-- <descriptor-set>]
    const example_mod = b.createModule(.{
        .root_source_file = b.path("examples/roundtrip.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{.{ .name = "pbwire", .module = pbwire_mod }},
    });
    const example = b.addExecutable(.{ .name = "roundtrip", .root_module = example_mod });
    example.root_module.linkLibrary(lib);
    const run_example = b.addRunArtifact(example);
    const example_step = b.step("example", "Run the round-trip example");
    example_step.dependOn(&run_example.step);

    // zig build bench [-- <matrix3.desc>]
    const bench_mod = b.createModule(.{
        .root_source_file = b.path("benchmarks/bench.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{
            .{ .name = "pbwire", .module = pbwire_mod },
            .{ .name = "pbwire_gen", .module = model_mod },
            .{ .name = "pbwire_core", .module = pure_mod },
        },
    });
    const bench = b.addExecutable(.{ .name = "bench", .root_module = bench_mod });
    bench.root_module.linkLibrary(lib);
    const run_bench = b.addRunArtifact(bench);
    const bench_step = b.step("bench", "Run the four standard tiers");
    bench_step.dependOn(&run_bench.step);

    // zig build testee (installs conformance/testee-zig). `-Dcore=true`
    // installs the pure-Zig testee instead: the official suite through the
    // no-native path, with no C module and no libpbwire.a.
    const testee_mod = b.createModule(.{
        .root_source_file = b.path(if (want_core) "conformance/testee_core.zig" else "conformance/testee.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = if (want_core)
            &.{.{ .name = "pbwire_core", .module = pure_mod }}
        else
            &.{.{ .name = "pbwire", .module = pbwire_mod }},
    });
    const testee = b.addExecutable(.{ .name = "testee-zig", .root_module = testee_mod });
    if (!want_core) testee.root_module.linkLibrary(lib);
    b.installArtifact(testee);
    const testee_step = b.step("testee", "Build the conformance testee");
    // Install, not just compile: the conformance target runs
    // zig-out/bin/testee-zig.
    testee_step.dependOn(&b.addInstallArtifact(testee, .{}).step);

    // Baseline comparison (opt-in: fetching the package needs network once,
    // then it is cached like any dependency). `zig build test` and the plain
    // `bench` step never reach `b.dependency` here, so they stay offline.
    //
    //   zig build -Doptimize=ReleaseFast -Dbaseline=true bench-baseline [-- <matrix3.desc>]
    const want_baseline = b.option(bool, "baseline", "Compare against Arwalk/zig-protobuf (fetches the package on first run)") orelse false;
    if (want_baseline) {
        const zp = b.dependency("zig_protobuf", .{ .target = target, .optimize = optimize });
        const protobuf_mod = zp.module("protobuf");
        const generator = zp.artifact("protoc-gen-zig");

        // System protoc drives their plugin (no protoc download involved).
        const protoc = b.findProgram(.{ .names = &.{ "protoc" } }) orelse "protoc";
        const gen_run = b.addSystemCommand(&.{protoc});
        gen_run.addPrefixedArtifactArg("--plugin=protoc-gen-zig=", generator);
        const gen_dir = gen_run.addPrefixedOutputDirectoryArg("--zig_out=", "gen");
        gen_run.addArg("-I");
        gen_run.addDirectoryArg(b.path("../c/tests/data"));
        gen_run.addFileArg(b.path("../c/tests/data/matrix3.proto"));
        const matrix3_mod = b.createModule(.{
            .root_source_file = gen_dir.path(b, "structtest.pb.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "protobuf", .module = protobuf_mod }},
        });

        const vs_mod = b.createModule(.{
            .root_source_file = b.path("benchmarks/bench_vs_baseline.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "pbwire", .module = pbwire_mod },
                .{ .name = "pbwire_gen", .module = model_mod },
                .{ .name = "protobuf", .module = protobuf_mod },
                .{ .name = "matrix3", .module = matrix3_mod },
            },
        });
        const vs = b.addExecutable(.{ .name = "bench-vs-baseline", .root_module = vs_mod });
        vs.root_module.linkLibrary(lib);
        const run_vs = b.addRunArtifact(vs);
        run_vs.addFileArg(b.path("../c/tests/data/matrix3.desc"));
        const vs_step = b.step("bench-baseline", "Run the four standard tiers vs Arwalk/zig-protobuf");
        vs_step.dependOn(&run_vs.step);
    }
}
