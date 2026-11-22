// Public surface of the pure-Dart core: a [Schema] loaded from a
// FileDescriptorSet, message types that decode and encode wire bytes, and the
// plain-map helpers the pbench core arm uses. No native library, no protoc
// plugin, no generated code, no package dependency -- and no `dart:io`, so it
// runs on every Dart target, including dart2js/dart2wasm where `dart:ffi`
// does not exist. Read the descriptor bytes with the caller's own IO:
//
//   final schema = Schema.fromBytes(File(path).readAsBytesSync());
//   final type = schema.message('structtest.Scalars3');
//   final msg = type.decode(bytes);      // unknown fields retained
//   type.encode(msg);                    // byte-exact for a decoded tree

import 'dart:typed_data';

import 'descriptor.dart';
import 'decode.dart' as dec;
import 'encode.dart' as enc;
import 'tree.dart';

export 'descriptor.dart'
    show
        CoreSchema,
        EnumDef,
        Field,
        K,
        Limits,
        MsgDef,
        defaultLimits,
        fieldByName,
        fieldCount,
        presenceExplicit,
        presenceImplicit,
        presenceMessage,
        presenceRequired;
export 'decode.dart' show decode, decodeSlice, newMessage;
export 'encode.dart' show encode;
export 'tree.dart'
    show
        Msg,
        Raw,
        fromObject,
        getField,
        hasField,
        oneofCase,
        rawPayload,
        toObject;
export 'wire.dart' show CoreError;

/// One message type: newMessage() plus binary decode/encode.
final class MessageType {
  MessageType(this.def);

  final MsgDef def;

  /// Fully qualified name, e.g. "structtest.Scalars3".
  String name() => def.fullName;

  /// An empty, mutable message of this type.
  Msg newMessage() => dec.newMessage(def);

  /// Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
  /// the authority on presence, so a field absent on the wire stays absent.
  Msg decode(Uint8List wire, [Limits? limits]) =>
      dec.decode(def, wire, limits);

  /// Decodes one body slice (offset + length) of a larger buffer.
  Msg decodeSlice(Uint8List wire, int off, int len, [Limits? limits]) =>
      dec.decodeSlice(def, wire, off, len, limits);

  /// Encodes a tree; a tree that came from decode() re-emits byte-exactly.
  Uint8List encode(Msg m) => enc.encode(m);

  /// Decodes straight into a plain map (proto field names, BigInt for 64-bit).
  Map<String, Object?> decodeObject(Uint8List wire, [Limits? limits]) =>
      toObject(decode(wire, limits));

  /// Encodes a plain map (the inverse of decodeObject).
  Uint8List encodeObject(Map<String, Object?> obj) =>
      encode(fromObject(def, obj));

  @override
  String toString() => name();
}

/// A loaded FileDescriptorSet: the runtime schema every type comes from.
final class Schema {
  Schema._(this.core);

  final CoreSchema core;

  /// Loads descriptor-set bytes; use --include_imports so every type resolves.
  factory Schema.fromBytes(Uint8List descriptorSet, [Limits? limits]) =>
      Schema._(loadSchema(descriptorSet, limits));

  /// Looks up a message by its fully qualified name.
  MessageType message(String fullName) => MessageType(core.message(fullName));

  /// Looks up an enum by its fully qualified name, or null.
  EnumDef? enumType(String fullName) => core.enums[fullName];
}
