// Interleaved timing for the Dart arms: per round `tree`, `shim`, `core` and
// the official protobuf-dart baseline run back to back on the same payload;
// per tier the median of five rounds is printed. The gate runs first in spirit:
// each round re-encodes with all three arms and refuses a row whose bytes
// differ from the official encoder.
//
//   dart run compare.dart [gen/matrix3.desc]
//
// `tree` is the whole user task (build the handle with one `set` per field,
// then encode; decode, then read every field back). `shim` is one
// `PbwireLayout.encode` / `decodeInto` call on the flat image, with the
// destination image and arena allocated once (the steady state the C core's
// arena is built for). `core` is the same whole user task as `tree`, with the
// pure-Dart codec instead of the C one.

import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:pbwire/pbwire.dart';

import 'a-tree.dart' as tree;
import 'b-shim.dart' as shim;
import 'c-core.dart' as core;
import 'payload.dart';

const _rounds = 5;

int _median(List<int> samples) {
  final sorted = List<int>.from(samples)..sort();
  return sorted[sorted.length ~/ 2];
}

int _measure(int iterations, void Function() op) {
  final watch = Stopwatch()..start();
  for (var i = 0; i < iterations; i++) {
    op();
  }
  return watch.elapsedMicroseconds * 1000 ~/ iterations;
}

String _row(String tier, String arm, int bytes, int enc, int dec) =>
    '${tier.padRight(7)} ${arm.padRight(14)} '
    '${enc.toString().padLeft(8)} ${(bytes / enc * 1000.0).toStringAsFixed(1).padLeft(8)} '
    '${dec.toString().padLeft(8)} ${(bytes / dec * 1000.0).toStringAsFixed(1).padLeft(8)}';

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : 'gen/matrix3.desc';
  final p = Pbench(desc);
  final arena = Arena();
  stdout.writeln('tier    arm              encode ns      MB/s  decode ns      MB/s');
  try {
    for (final tier in tiers) {
      final official = p.official(tier.repeats);
      final wire = official.writeToBuffer();
      if (wire.length != tier.bytes) {
        stderr.writeln('${tier.label}: ${wire.length} bytes, want ${tier.bytes}');
        exit(1);
      }
      final image = p.image(arena, tier.repeats);
      final out = shim.output();
      final decodeArena = PbwArena(wire.length);
      // Warm every op once, then check the bytes each arm would publish.
      tree.encode(p, tier.repeats);
      shim.encode(p, image);
      core.encode(p, tier.repeats);
      if (!equals(shim.encode(p, image), wire) ||
          !equals(tree.encode(p, tier.repeats), wire) ||
          !equals(core.encode(p, tier.repeats), wire)) {
        stderr.writeln('${tier.label}: an arm disagrees with the official bytes');
        exit(1);
      }
      final treeEnc = <int>[];
      final shimEnc = <int>[];
      final coreEnc = <int>[];
      final officialEnc = <int>[];
      final treeDec = <int>[];
      final shimDec = <int>[];
      final coreDec = <int>[];
      final officialDec = <int>[];
      try {
        for (var round = 0; round < _rounds; round++) {
          treeEnc.add(_measure(tier.iterations, () => tree.encode(p, tier.repeats)));
          shimEnc.add(_measure(
              tier.iterations, () => PbwireLayout.encode(image, p.imageType)));
          coreEnc.add(_measure(tier.iterations, () => core.encode(p, tier.repeats)));
          officialEnc.add(_measure(tier.iterations,
              () => p.official(tier.repeats).writeToBuffer()));
          treeDec.add(_measure(tier.iterations, () {
            tree.decode(p, wire, tier.repeats);
          }));
          shimDec.add(_measure(tier.iterations, () {
            shim.decode(p, wire, out, decodeArena);
          }));
          coreDec.add(_measure(tier.iterations, () {
            core.decode(p, wire, tier.repeats);
          }));
          officialDec.add(
              _measure(tier.iterations, () => p.officialFromBuffer(wire)));
        }
        final te = _median(treeEnc);
        final se = _median(shimEnc);
        final ce = _median(coreEnc);
        final oe = _median(officialEnc);
        final td = _median(treeDec);
        final sd = _median(shimDec);
        final cd = _median(coreDec);
        final od = _median(officialDec);
        stdout.writeln(_row(tier.label, 'tree', wire.length, te, td));
        stdout.writeln(_row(tier.label, 'shim', wire.length, se, sd));
        stdout.writeln(_row(tier.label, 'core', wire.length, ce, cd));
        stdout.writeln(_row(tier.label, 'protobuf-dart', wire.length, oe, od));
        if (!shim.read(out, tier.repeats)) {
          stderr.writeln('${tier.label}: shim decode did not expose the fields');
          exit(1);
        }
      } finally {
        decodeArena.release();
        calloc.free(out);
      }
      stdout.writeln();
    }
  } finally {
    arena.releaseAll();
    p.close();
  }
}
