// The `shim` arm: the generated `Scalars3Layout` image is the native object;
// `PbwireLayout.encode` / `decodeInto` cross once and the C reader
// (bindings/dart/native/pbw_dart_shim.c) walks that memory in place.

import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:pbwire/pbwire.dart';

import 'gen/matrix3.dart' as pbw;
import 'payload.dart';

Uint8List encode(Pbench p, Pointer<pbw.Scalars3Layout> image) =>
    PbwireLayout.encode(image, p.imageType);

void decode(Pbench p, Uint8List wire, Pointer<pbw.Scalars3Layout> into,
        PbwArena arena) =>
    PbwireLayout.decodeInto(wire, into, p.imageType, arena);

/// Reads the decoded image; the gate uses this, the timer does not (the C
/// reader already wrote the native object).
bool read(Pointer<pbw.Scalars3Layout> out, int repeats) {
  final inner = out.ref.inner;
  final ok = out.ref.i32 == -2147483648 &&
      out.ref.i64 == 9223372036854775807 &&
      out.ref.u64 == -1 &&
      out.ref.fx64 == 72623859790382856 &&
      out.ref.f32 == double.infinity &&
      out.ref.f64 == -123456.789 &&
      out.ref.flag &&
      out.ref.str.toDartString() == 'hello' &&
      out.ref.color == 2 &&
      out.ref.hasOptI32 == 1 &&
      out.ref.optI32 == -7 &&
      out.ref.optStr.toDartString() == 'opt' &&
      out.ref.numsCount == repeats &&
      out.ref.tagsCount == repeats &&
      out.ref.innersCount == repeats &&
      out.ref.flagsCount == repeats &&
      (repeats == 0 ||
          (out.ref.nums[repeats - 1] == repeats - 4 &&
              out.ref.tags[repeats - 1].toDartString() == 'tag' &&
              out.ref.flags[repeats - 1] == ((repeats - 1) % 2 == 0)));
  final kid = inner.ref.name.toDartString() == 'kid' && inner.ref.value == 42;
  return ok && kid;
}

/// An empty output image (fresh zeroed memory; decode writes into it).
Pointer<pbw.Scalars3Layout> output() =>
    calloc<Uint8>(sizeOf<pbw.Scalars3Layout>()).cast<pbw.Scalars3Layout>();
