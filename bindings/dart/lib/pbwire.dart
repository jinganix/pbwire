/*
 * The Dart binding: dart:ffi over the C ABI in include/pbwire.h. A
 * FileDescriptorSet (protoc --descriptor_set_out) becomes the schema at run
 * time; there is no generated code and no C glue. The wire format lives in the
 * C core, so a Dart message and a C message are the same bytes by construction.
 *
 * Ownership: a Schema or a root Message owns native memory and is released by
 * close() (or the finalizer); a nested message is a view of its parent and
 * never frees anything. One message belongs to one isolate: dart:ffi calls are
 * per-isolate and the tree is not thread safe, like the C core.
 *
 * The library is PBWIRE_LIB, else build/libpbwire.{dylib,so} next to the
 * checkout (`make shared` builds it).
 */

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// Anything the C core refused, plus I/O failures from [Schema.load].
final class PbwireException implements Exception {
  PbwireException(this.status, this.message);

  /// The `pbw_status` value, or -1 for an I/O failure.
  final int status;
  final String message;

  @override
  String toString() => message;
}

/* --------------------------------------------------------------- the library */

typedef _LoadNative = Int32 Function(
    Pointer<Void> data, IntPtr len, Pointer<Pointer<Void>> out);
typedef _LoadDart = int Function(
    Pointer<Void> data, int len, Pointer<Pointer<Void>> out);
typedef _FreeNative = Void Function(Pointer<Void> ptr);
typedef _FreeDart = void Function(Pointer<Void> ptr);
typedef _SchemaFreeNative = Void Function(Pointer<Void> schema);
typedef _SchemaFreeDart = void Function(Pointer<Void> schema);
typedef _SchemaErrorNative = Pointer<Utf8> Function(Pointer<Void> schema);
typedef _SchemaErrorDart = Pointer<Utf8> Function(Pointer<Void> schema);
typedef _SchemaFindNative = Pointer<Void> Function(
    Pointer<Void> schema, Pointer<Utf8> name);
typedef _SchemaFindDart = Pointer<Void> Function(
    Pointer<Void> schema, Pointer<Utf8> name);
typedef _FindFieldNative = Pointer<Void> Function(
    Pointer<Void> desc, Pointer<Utf8> name);
typedef _FindFieldDart = Pointer<Void> Function(
    Pointer<Void> desc, Pointer<Utf8> name);
typedef _NameNative = Pointer<Utf8> Function(Pointer<Void> ptr);
typedef _NameDart = Pointer<Utf8> Function(Pointer<Void> ptr);
typedef _PtrFnNative = Pointer<Void> Function(Pointer<Void> ptr);
typedef _PtrFnDart = Pointer<Void> Function(Pointer<Void> ptr);
typedef _Uint32Native = Uint32 Function(Pointer<Void> ptr);
typedef _Uint32Dart = int Function(Pointer<Void> ptr);
typedef _Int32Native = Int32 Function(Pointer<Void> ptr);
typedef _Int32Dart = int Function(Pointer<Void> ptr);
typedef _MsgNewNative = Pointer<Void> Function(Pointer<Void> desc);
typedef _MsgNewDart = Pointer<Void> Function(Pointer<Void> desc);
typedef _MsgGetNative = Pointer<Void> Function(Pointer<Void> msg, Pointer<Void> field);
typedef _MsgGetDart = Pointer<Void> Function(Pointer<Void> msg, Pointer<Void> field);
typedef _MsgSetNative = Int32 Function(
    Pointer<Void> msg, Pointer<Void> field, Pointer<Void> value);
typedef _MsgSetDart = int Function(
    Pointer<Void> msg, Pointer<Void> field, Pointer<Void> value);
typedef _MsgSetMessageNative = Pointer<Void> Function(Pointer<Void> msg, Uint32 number);
typedef _MsgSetMessageDart = Pointer<Void> Function(Pointer<Void> msg, int number);
typedef _MsgClearNative = Void Function(Pointer<Void> msg, Uint32 number);
typedef _MsgClearDart = void Function(Pointer<Void> msg, int number);
typedef _ValueNewIntNative = Pointer<Void> Function(Int64 value);
typedef _ValueNewIntDart = Pointer<Void> Function(int value);
typedef _ValueNewInt32Native = Pointer<Void> Function(Int32 value);
typedef _ValueNewInt32Dart = Pointer<Void> Function(int value);
typedef _ValueNewF32Native = Pointer<Void> Function(Float value);
typedef _ValueNewF32Dart = Pointer<Void> Function(double value);
typedef _ValueNewF64Native = Pointer<Void> Function(Double value);
typedef _ValueNewF64Dart = Pointer<Void> Function(double value);
typedef _ValueNewBytesNative = Pointer<Void> Function(Pointer<Void> data, IntPtr len);
typedef _ValueNewBytesDart = Pointer<Void> Function(Pointer<Void> data, int len);
typedef _ValueNewStringNative = Pointer<Void> Function(Pointer<Utf8> str);
typedef _ValueNewStringDart = Pointer<Void> Function(Pointer<Utf8> str);
typedef _ValueIntNative = Int64 Function(Pointer<Void> value);
typedef _ValueIntDart = int Function(Pointer<Void> value);
typedef _ValueInt32Native = Int32 Function(Pointer<Void> value);
typedef _ValueInt32Dart = int Function(Pointer<Void> value);
typedef _ValueF32Native = Float Function(Pointer<Void> value);
typedef _ValueF32Dart = double Function(Pointer<Void> value);
typedef _ValueF64Native = Double Function(Pointer<Void> value);
typedef _ValueF64Dart = double Function(Pointer<Void> value);
typedef _ValueBytesNative = Pointer<Void> Function(
    Pointer<Void> value, Pointer<IntPtr> len);
typedef _ValueBytesDart = Pointer<Void> Function(
    Pointer<Void> value, Pointer<IntPtr> len);
typedef _ValueStringNative = Pointer<Utf8> Function(Pointer<Void> value);
typedef _ValueStringDart = Pointer<Utf8> Function(Pointer<Void> value);
typedef _ValueMessageNative = Pointer<Void> Function(Pointer<Void> value);
typedef _ValueMessageDart = Pointer<Void> Function(Pointer<Void> value);
typedef _ValueCountNative = IntPtr Function(Pointer<Void> value);
typedef _ValueCountDart = int Function(Pointer<Void> value);
typedef _ValueAtNative = Pointer<Void> Function(Pointer<Void> value, IntPtr index);
typedef _ValueAtDart = Pointer<Void> Function(Pointer<Void> value, int index);
typedef _EncodeNative = Int32 Function(
    Pointer<Void> msg, Pointer<Pointer<Uint8>> out, Pointer<IntPtr> len);
typedef _EncodeDart = int Function(
    Pointer<Void> msg, Pointer<Pointer<Uint8>> out, Pointer<IntPtr> len);
typedef _EncodeToNative = Int32 Function(
    Pointer<Void> msg, Pointer<Void> out, IntPtr cap, Pointer<IntPtr> len);
typedef _EncodeToDart = int Function(
    Pointer<Void> msg, Pointer<Void> out, int cap, Pointer<IntPtr> len);
typedef _DecodeNative = Int32 Function(Pointer<Void> desc, Pointer<Void> buf,
    IntPtr len, Pointer<Pointer<Void>> out);
typedef _DecodeDart = int Function(Pointer<Void> desc, Pointer<Void> buf,
    int len, Pointer<Pointer<Void>> out);
typedef _StrerrorNative = Pointer<Utf8> Function(Int32 status);
typedef _StrerrorDart = Pointer<Utf8> Function(int status);

/// The dlopen()ed library, one lookup per exported function.
final class PbwireLibrary {
  PbwireLibrary._(DynamicLibrary lib)
      : load = lib.lookupFunction<_LoadNative, _LoadDart>('pbw_descriptor_set_load'),
        free = lib.lookupFunction<_FreeNative, _FreeDart>('pbw_free'),
        schemaFree = lib.lookupFunction<_SchemaFreeNative, _SchemaFreeDart>('pbw_schema_free'),
        schemaError =
            lib.lookupFunction<_SchemaErrorNative, _SchemaErrorDart>('pbw_schema_error'),
        schemaFind = lib.lookupFunction<_SchemaFindNative, _SchemaFindDart>('pbw_schema_find'),
        descriptorName =
            lib.lookupFunction<_NameNative, _NameDart>('pbw_descriptor_name'),
        findField =
            lib.lookupFunction<_FindFieldNative, _FindFieldDart>('pbw_descriptor_find_field_name'),
        fieldName = lib.lookupFunction<_NameNative, _NameDart>('pbw_field_name'),
        fieldNumber = lib.lookupFunction<_Uint32Native, _Uint32Dart>('pbw_field_number'),
        fieldType = lib.lookupFunction<_Int32Native, _Int32Dart>('pbw_field_type'),
        fieldLabel = lib.lookupFunction<_Int32Native, _Int32Dart>('pbw_field_label'),
        fieldMessageType =
            lib.lookupFunction<_PtrFnNative, _PtrFnDart>('pbw_field_message_type'),
        messageNew = lib.lookupFunction<_MsgNewNative, _MsgNewDart>('pbw_message_new'),
        messageFree = lib.lookupFunction<_SchemaFreeNative, _SchemaFreeDart>('pbw_message_free'),
        messageGet = lib.lookupFunction<_MsgGetNative, _MsgGetDart>('pbw_message_get_field'),
        messageSet = lib.lookupFunction<_MsgSetNative, _MsgSetDart>('pbw_message_set_field'),
        messageSetMessage =
            lib.lookupFunction<_MsgSetMessageNative, _MsgSetMessageDart>('pbw_message_set_message'),
        messageClear = lib.lookupFunction<_MsgClearNative, _MsgClearDart>('pbw_message_clear'),
        valueNewInt = lib.lookupFunction<_ValueNewIntNative, _ValueNewIntDart>('pbw_value_new_int'),
        valueNewBool =
            lib.lookupFunction<_ValueNewInt32Native, _ValueNewInt32Dart>('pbw_value_new_bool'),
        valueNewFloat =
            lib.lookupFunction<_ValueNewF32Native, _ValueNewF32Dart>('pbw_value_new_float'),
        valueNewDouble =
            lib.lookupFunction<_ValueNewF64Native, _ValueNewF64Dart>('pbw_value_new_double'),
        valueNewBytes =
            lib.lookupFunction<_ValueNewBytesNative, _ValueNewBytesDart>('pbw_value_new_bytes'),
        valueNewString =
            lib.lookupFunction<_ValueNewStringNative, _ValueNewStringDart>('pbw_value_new_string'),
        valueFree = lib.lookupFunction<_SchemaFreeNative, _SchemaFreeDart>('pbw_value_free'),
        valueInt = lib.lookupFunction<_ValueIntNative, _ValueIntDart>('pbw_value_get_int'),
        valueBool = lib.lookupFunction<_ValueInt32Native, _ValueInt32Dart>('pbw_value_get_bool'),
        valueFloat = lib.lookupFunction<_ValueF32Native, _ValueF32Dart>('pbw_value_get_float'),
        valueDouble = lib.lookupFunction<_ValueF64Native, _ValueF64Dart>('pbw_value_get_double'),
        valueBytes = lib.lookupFunction<_ValueBytesNative, _ValueBytesDart>('pbw_value_get_bytes'),
        valueString =
            lib.lookupFunction<_ValueStringNative, _ValueStringDart>('pbw_value_get_string'),
        valueMessage =
            lib.lookupFunction<_ValueMessageNative, _ValueMessageDart>('pbw_value_get_message'),
        valueCount = lib.lookupFunction<_ValueCountNative, _ValueCountDart>('pbw_value_count'),
        valueAt = lib.lookupFunction<_ValueAtNative, _ValueAtDart>('pbw_value_at'),
        encode = lib.lookupFunction<_EncodeNative, _EncodeDart>('pbw_encode'),
        encodeTo = lib.lookupFunction<_EncodeToNative, _EncodeToDart>('pbw_encode_to'),
        decode = lib.lookupFunction<_DecodeNative, _DecodeDart>('pbw_decode'),
        strerror = lib.lookupFunction<_StrerrorNative, _StrerrorDart>('pbw_strerror');

  static PbwireLibrary? _instance;

  /// The process-wide library handle; Dart has no per-thread state to keep.
  static PbwireLibrary get instance => _instance ??= PbwireLibrary._(DynamicLibrary.open(path()));

  /// PBWIRE_LIB, else build/libpbwire.{dylib,so} found by walking up from the
  /// working directory or from the entry script.
  static String path() {
    final env = Platform.environment['PBWIRE_LIB'];
    if (env != null && env.isNotEmpty) {
      return env;
    }
    for (final start in [Directory.current.path, File.fromUri(Platform.script).parent.path]) {
      var dir = Directory(start).absolute;
      while (true) {
        for (final suffix in ['dylib', 'so']) {
          final candidate = File('${dir.path}/build/libpbwire.$suffix');
          if (candidate.existsSync()) {
            return candidate.path;
          }
        }
        final parent = dir.parent;
        if (parent.path == dir.path) {
          break;
        }
        dir = parent;
      }
    }
    throw PbwireException(-1, 'no shared library found (set PBWIRE_LIB; run `make shared`)');
  }

  final _LoadDart load;
  final _FreeDart free;
  final _SchemaFreeDart schemaFree;
  final _SchemaErrorDart schemaError;
  final _SchemaFindDart schemaFind;
  final _NameDart descriptorName;
  final _FindFieldDart findField;
  final _NameDart fieldName;
  final _Uint32Dart fieldNumber;
  final _Int32Dart fieldType;
  final _Int32Dart fieldLabel;
  final _PtrFnDart fieldMessageType;
  final _MsgNewDart messageNew;
  final _SchemaFreeDart messageFree;
  final _MsgGetDart messageGet;
  final _MsgSetDart messageSet;
  final _MsgSetMessageDart messageSetMessage;
  final _MsgClearDart messageClear;
  final _ValueNewIntDart valueNewInt;
  final _ValueNewInt32Dart valueNewBool;
  final _ValueNewF32Dart valueNewFloat;
  final _ValueNewF64Dart valueNewDouble;
  final _ValueNewBytesDart valueNewBytes;
  final _ValueNewStringDart valueNewString;
  final _SchemaFreeDart valueFree;
  final _ValueIntDart valueInt;
  final _ValueInt32Dart valueBool;
  final _ValueF32Dart valueFloat;
  final _ValueF64Dart valueDouble;
  final _ValueBytesDart valueBytes;
  final _ValueStringDart valueString;
  final _ValueMessageDart valueMessage;
  final _ValueCountDart valueCount;
  final _ValueAtDart valueAt;
  final _EncodeDart encode;
  final _EncodeToDart encodeTo;
  final _DecodeDart decode;
  final _StrerrorDart strerror;
}

PbwireLibrary get _lib => PbwireLibrary.instance;

PbwireException _statusError(int status) =>
    PbwireException(status, _lib.strerror(status).toDartString());

/// Copies Dart bytes into native memory for one call.
Pointer<Uint8> _copyIn(List<int> bytes) {
  if (bytes.isEmpty) {
    return nullptr.cast();
  }
  final buf = malloc<Uint8>(bytes.length);
  buf.asTypedList(bytes.length).setAll(0, bytes);
  return buf;
}

/* ---------------------------------------------------------------- the schema */

/// A loaded FileDescriptorSet: the schema every [Type] comes from.
final class Schema {
  Schema._(this._handle) {
    _finalizer.attach(this, _handle, detach: this);
  }

  static final _finalizer = Finalizer<Pointer<Void>>(_lib.schemaFree);

  Pointer<Void> _handle;

  /// Loads a descriptor set written by `protoc --descriptor_set_out`.
  factory Schema.load(String path) {
    final file = File(path);
    if (!file.existsSync()) {
      throw PbwireException(-1, 'cannot read $path');
    }
    return Schema.fromBytes(file.readAsBytesSync());
  }

  /// Loads descriptor-set bytes; use `--include_imports` so every type resolves.
  factory Schema.fromBytes(List<int> bytes) {
    final data = _copyIn(bytes);
    final out = calloc<Pointer<Void>>();
    try {
      final status = _lib.load(data.cast(), bytes.length, out);
      final handle = out.value;
      if (status != 0) {
        final why = handle == nullptr ? '' : _lib.schemaError(handle).toDartString();
        if (handle != nullptr) {
          _lib.schemaFree(handle);
        }
        throw PbwireException(status, why.isEmpty ? _statusError(status).message : why);
      }
      if (handle == nullptr) {
        throw PbwireException(-1, 'descriptor set: no schema returned');
      }
      return Schema._(handle);
    } finally {
      if (data != nullptr) {
        malloc.free(data);
      }
      calloc.free(out);
    }
  }

  /// Looks up a message by fully qualified name, e.g. `structtest.Scalars3`.
  Type message(String fullName) {
    final name = fullName.toNativeUtf8();
    try {
      final desc = _lib.schemaFind(_handle, name);
      if (desc == nullptr) {
        throw PbwireException(-1, 'no such message type: $fullName');
      }
      return Type._(this, desc);
    } finally {
      malloc.free(name);
    }
  }

  void close() {
    if (_handle != nullptr) {
      _finalizer.detach(this);
      _lib.schemaFree(_handle);
      _handle = nullptr;
    }
  }
}

/* ---------------------------------------------------------------- the type */

/// A message type from a [Schema].
final class Type {
  Type._(this._schema, this._desc);

  final Schema _schema;
  final Pointer<Void> _desc;
  final Map<String, Field> _fields = {};

  String get name => _lib.descriptorName(_desc).toDartString();

  /// Resolves a field once and caches it: a later set/get costs one native call.
  Field field(String name) {
    final cached = _fields[name];
    if (cached != null) {
      return cached;
    }
    final native = name.toNativeUtf8();
    try {
      final ptr = _lib.findField(_desc, native);
      if (ptr == nullptr) {
        throw PbwireException(-1, '$name: no such field on ${this.name}');
      }
      return _fields[name] = Field._(ptr);
    } finally {
      malloc.free(native);
    }
  }

  /// A new empty message; freed by [Message.close] (or the finalizer).
  Message newMessage() {
    final ptr = _lib.messageNew(_desc);
    if (ptr == nullptr) {
      throw PbwireException(-1, 'pbw_message_new: out of memory');
    }
    return Message._root(this, ptr);
  }

  /// Decodes wire bytes; unknown fields are preserved and re-encoded.
  Message decode(List<int> wire) {
    final data = _copyIn(wire);
    final out = calloc<Pointer<Void>>();
    try {
      final status = _lib.decode(_desc, data.cast(), wire.length, out);
      if (status != 0) {
        throw _statusError(status);
      }
      if (out.value == nullptr) {
        throw PbwireException(-1, 'decode: no message returned');
      }
      return Message._root(this, out.value);
    } finally {
      if (data != nullptr) {
        malloc.free(data);
      }
      calloc.free(out);
    }
  }

  /// The message type of a MESSAGE/GROUP field, for nested access.
  Type? nestedType(Field field) {
    final desc = _lib.fieldMessageType(field._ptr);
    return desc == nullptr ? null : Type._(_schema, desc);
  }
}

/// A resolved field handle.
final class Field {
  Field._(this._ptr);

  final Pointer<Void> _ptr;

  String get name => _lib.fieldName(_ptr).toDartString();
  int get number => _lib.fieldNumber(_ptr);
  int get type => _lib.fieldType(_ptr);

  /// 3 for repeated.
  int get label => _lib.fieldLabel(_ptr);
}

/* --------------------------------------------------------------- the message */

/// One dynamic message. A root owns its tree; a nested message borrows it.
final class Message {
  Message._(this._type, this._handle, this._owned, this._parent) {
    if (_owned) {
      _finalizer.attach(this, _handle, detach: this);
    }
  }

  static final _finalizer = Finalizer<Pointer<Void>>(_lib.messageFree);

  final Type _type;

  /// Keeps the parent (and its tree) alive while a nested message is in use.
  // ignore: unused_field
  final Message? _parent;
  final bool _owned;
  Pointer<Void> _handle;

  static Message _root(Type type, Pointer<Void> handle) =>
      Message._(type, handle, true, null);

  static Message _view(Type type, Pointer<Void> handle, Message parent) =>
      Message._(type, handle, false, parent);

  Type get type => _type;

  /* ---------------------------------------------------------------- setting */

  /// Any integer or enum field; the value is stored as raw 64-bit bits.
  Message setInt(Object field, int value) =>
      _store(_resolve(field), _lib.valueNewInt(value));

  Message setBool(Object field, bool value) =>
      _store(_resolve(field), _lib.valueNewBool(value ? 1 : 0));

  Message setFloat(Object field, double value) =>
      _store(_resolve(field), _lib.valueNewFloat(value));

  Message setDouble(Object field, double value) =>
      _store(_resolve(field), _lib.valueNewDouble(value));

  Message setString(Object field, String value) {
    final native = value.toNativeUtf8();
    try {
      return _store(_resolve(field), _lib.valueNewString(native));
    } finally {
      malloc.free(native);
    }
  }

  Message setBytes(Object field, List<int> value) {
    final data = _copyIn(value);
    try {
      return _store(_resolve(field), _lib.valueNewBytes(data.cast(), value.length));
    } finally {
      if (data != nullptr) {
        malloc.free(data);
      }
    }
  }

  /// Nested message for a MESSAGE field, created in place (or returned as it is).
  Message setMessage(Object field) {
    final resolved = _resolve(field);
    final ptr = _lib.messageSetMessage(_handle, resolved.number);
    final type = _type.nestedType(resolved);
    if (ptr == nullptr || type == null) {
      throw PbwireException(-1, 'set ${resolved.name}: not a message field');
    }
    return Message._view(type, ptr, this);
  }

  /// Appends to a repeated field or replaces a singular one.
  Message _store(Field field, Pointer<Void> value) {
    if (value == nullptr) {
      throw PbwireException(-1, 'set ${field.name}: out of memory');
    }
    final status = _lib.messageSet(_handle, field._ptr, value);
    if (status != 0) {
      /* A rejected value stays owned by the caller. */
      _lib.valueFree(value);
      throw PbwireException(status, 'set ${field.name}: ${_statusError(status).message}');
    }
    return this;
  }

  void clear(Object field) => _lib.messageClear(_handle, _resolve(field).number);

  /* ---------------------------------------------------------------- reading */

  Pointer<Void>? _value(Field field) {
    final ptr = _lib.messageGet(_handle, field._ptr);
    return ptr == nullptr ? null : ptr;
  }

  bool has(Object field) => _value(_resolve(field)) != null;

  int getInt(Object field) {
    final value = _value(_resolve(field));
    return value == null ? 0 : _lib.valueInt(value);
  }

  bool getBool(Object field) {
    final value = _value(_resolve(field));
    return value != null && _lib.valueBool(value) != 0;
  }

  double getFloat(Object field) {
    final value = _value(_resolve(field));
    return value == null ? 0.0 : _lib.valueFloat(value);
  }

  double getDouble(Object field) {
    final value = _value(_resolve(field));
    return value == null ? 0.0 : _lib.valueDouble(value);
  }

  String? getString(Object field) {
    final value = _value(_resolve(field));
    if (value == null) {
      return null;
    }
    final ptr = _lib.valueString(value);
    return ptr == nullptr ? null : ptr.toDartString();
  }

  Uint8List? getBytes(Object field) {
    final value = _value(_resolve(field));
    if (value == null) {
      return null;
    }
    final len = malloc<IntPtr>();
    try {
      final ptr = _lib.valueBytes(value, len);
      if (ptr == nullptr) {
        return null;
      }
      return Uint8List.fromList(ptr.cast<Uint8>().asTypedList(len.value));
    } finally {
      malloc.free(len);
    }
  }

  Message? getMessage(Object field) {
    final resolved = _resolve(field);
    final value = _value(resolved);
    if (value == null) {
      return null;
    }
    final ptr = _lib.valueMessage(value);
    final type = _type.nestedType(resolved);
    if (ptr == nullptr || type == null) {
      return null;
    }
    return Message._view(type, ptr, this);
  }

  /// Items in a repeated field, or 0 when the field is absent.
  int count(Object field) {
    final value = _value(_resolve(field));
    return value == null ? 0 : _lib.valueCount(value);
  }

  /// One item of a repeated field.
  Value? at(Object field, int index) {
    final resolved = _resolve(field);
    final value = _value(resolved);
    if (value == null) {
      return null;
    }
    final item = _lib.valueAt(value, index);
    if (item == nullptr) {
      return null;
    }
    return Value._(item, this, _type.nestedType(resolved));
  }

  /* ------------------------------------------------------------------- wire */

  /// Encoded bytes (one allocation by the core, one copy into a Dart list).
  Uint8List encode() {
    final out = calloc<Pointer<Uint8>>();
    final len = malloc<IntPtr>();
    try {
      final status = _lib.encode(_handle, out, len);
      if (status != 0) {
        throw _statusError(status);
      }
      final bytes = Uint8List.fromList(out.value.asTypedList(len.value));
      _lib.free(out.value.cast());
      return bytes;
    } finally {
      calloc.free(out);
      malloc.free(len);
    }
  }

  void close() {
    if (_handle != nullptr && _owned) {
      _finalizer.detach(this);
      _lib.messageFree(_handle);
      _handle = nullptr;
    }
  }

  Field _resolve(Object field) => switch (field) {
        Field f => f,
        String name => _type.field(name),
        _ => throw ArgumentError.value(field, 'field', 'Field or field name expected'),
      };
}

/// A field value; only repeated items need it, hence the tiny surface.
final class Value {
  Value._(this._ptr, this._owner, this._type);

  final Pointer<Void> _ptr;
  final Message _owner;
  final Type? _type;

  int asInt() => _lib.valueInt(_ptr);
  bool asBool() => _lib.valueBool(_ptr) != 0;
  double asFloat() => _lib.valueFloat(_ptr);
  double asDouble() => _lib.valueDouble(_ptr);

  String? asString() {
    final ptr = _lib.valueString(_ptr);
    return ptr == nullptr ? null : ptr.toDartString();
  }

  Uint8List? asBytes() {
    final len = malloc<IntPtr>();
    try {
      final ptr = _lib.valueBytes(_ptr, len);
      return ptr == nullptr ? null : Uint8List.fromList(ptr.cast<Uint8>().asTypedList(len.value));
    } finally {
      malloc.free(len);
    }
  }

  Message? asMessage() {
    final ptr = _lib.valueMessage(_ptr);
    final type = _type;
    if (ptr == nullptr || type == null) {
      return null;
    }
    return Message._view(type, ptr, _owner);
  }
}
