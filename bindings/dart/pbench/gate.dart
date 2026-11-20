// The pbench gate: every arm must produce the official protobuf package's
// bytes on all four tiers, and every arm's decode must expose the fields
// payload.dart lists, before any number is printed.
//
//   dart run gate.dart [gen/matrix3.desc]

import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:pbwire/pbwire.dart';

import 'a-tree.dart' as tree;
import 'b-shim.dart' as shim;
import 'payload.dart';

var _checks = 0;
var _failures = 0;

void check(bool ok, String what) {
  _checks++;
  if (!ok) {
    _failures++;
    stdout.writeln('FAIL $what');
  }
}

Uint8List _official(Pbench p, int repeats) {
  final msg = p.official(repeats);
  final bytes = msg.writeToBuffer();
  check(bytes.length == tiers.firstWhere((t) => t.repeats == repeats).bytes,
      'official ${bytes.length} bytes at x$repeats');
  return bytes;
}

void main(List<String> args) {
  final desc = args.isNotEmpty ? args[0] : 'gen/matrix3.desc';
  final p = Pbench(desc);
  final arena = Arena();
  try {
    for (final tier in tiers) {
      final wire = _official(p, tier.repeats);
      final treeWire = tree.encode(p, tier.repeats);
      final image = p.image(arena, tier.repeats);
      final shimWire = shim.encode(p, image);
      check(equals(treeWire, wire), '${tier.label}: tree bytes differ');
      check(equals(shimWire, wire), '${tier.label}: shim bytes differ');

      // Decode each arm and read the same fields back.
      final treeAgain = tree.decode(p, wire, tier.repeats);
      check(treeAgain != null && equals(treeAgain, wire),
          '${tier.label}: tree decode/re-encode differs');

      final out = shim.output();
      final decodeArena = PbwArena(wire.length);
      try {
        shim.decode(p, wire, out, decodeArena);
        check(shim.read(out, tier.repeats), '${tier.label}: shim fields differ');
        check(equals(PbwireLayout.encode(out, p.imageType), wire),
            '${tier.label}: shim decode/re-encode differs');
      } finally {
        decodeArena.release();
        calloc.free(out);
      }

      // The official baseline decodes to the same values too.
      final officialBack = p.officialFromBuffer(wire);
      check(
          officialBack.str == 'hello' &&
              officialBack.inner.name == 'kid' &&
              officialBack.inner.value == 42 &&
              officialBack.nums.length == tier.repeats &&
              officialBack.inners.length == tier.repeats &&
              officialBack.optI32 == -7 &&
              officialBack.optStr == 'opt',
          '${tier.label}: official fields differ');
      // No official re-encode check: protobuf-dart 6.1's sint64 decoder reads
      // -9223372036854775808 as 0 (`Scalars3.fromBuffer(writeToBuffer())` shrinks
      // this payload by 9 bytes), so only the official encoder is an oracle.

      stdout.writeln('OK   ${tier.label}: ${wire.length} bytes, '
          'tree/${shimWire.length} shim, x${tier.repeats}');
    }
  } finally {
    arena.releaseAll();
    p.close();
  }
  if (_failures != 0) {
    stderr.writeln('gate: $_failures of $_checks checks failed');
    exit(1);
  }
  stdout.writeln('gate: PASS ($_checks checks)');
}
