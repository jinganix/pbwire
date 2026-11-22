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

import 'dart:convert';
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
typedef _DecodeDelimitedNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    IntPtr len,
    Pointer<Pointer<Void>> out,
    Pointer<IntPtr> consumed);
typedef _DecodeDelimitedDart = int Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    int len,
    Pointer<Pointer<Void>> out,
    Pointer<IntPtr> consumed);
typedef _DecodeLimitsNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    IntPtr len,
    Pointer<Void> limits,
    Pointer<Pointer<Void>> out);

/* pbw_visit_value / pbw_visit_fn: one scanned field, no tree. */
final class _VisitValue extends Struct {
  @Uint64()
  external int u64;

  external Pointer<Uint8> data;

  @Size()
  external int len;
}

typedef _VisitFnNative = Int32 Function(Pointer<Void> ud, Pointer<Void> field,
    Uint32 number, Uint32 wireType, Pointer<_VisitValue> value);
typedef _VisitNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    IntPtr len,
    Pointer<Void> limits,
    Pointer<NativeFunction<_VisitFnNative>> cb,
    Pointer<Void> ud,
    Pointer<IntPtr> failOff);
typedef _DecodeLimitsDart = int Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    int len,
    Pointer<Void> limits,
    Pointer<Pointer<Void>> out);
typedef _VisitDart = int Function(
    Pointer<Void> desc,
    Pointer<Void> buf,
    int len,
    Pointer<Void> limits,
    Pointer<NativeFunction<_VisitFnNative>> cb,
    Pointer<Void> ud,
    Pointer<IntPtr> failOff);
typedef _JsonParseNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    IntPtr len,
    Pointer<Void> opts,
    Pointer<Pointer<Void>> out);
typedef _JsonParseDart = int Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    int len,
    Pointer<Void> opts,
    Pointer<Pointer<Void>> out);
typedef _JsonParseLimitsNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    IntPtr len,
    Pointer<Void> limits,
    Pointer<Void> opts,
    Pointer<Pointer<Void>> out);
typedef _JsonParseLimitsDart = int Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    int len,
    Pointer<Void> limits,
    Pointer<Void> opts,
    Pointer<Pointer<Void>> out);
typedef _TextParseLimitsNative = Int32 Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    IntPtr len,
    Pointer<Void> limits,
    Pointer<Pointer<Void>> out);
typedef _TextParseLimitsDart = int Function(
    Pointer<Void> desc,
    Pointer<Utf8> text,
    int len,
    Pointer<Void> limits,
    Pointer<Pointer<Void>> out);
typedef _TextPrintOptsNative = Int32 Function(Pointer<Void> msg, Int32 printUnknown,
    Pointer<Pointer<Uint8>> out, Pointer<IntPtr> len);
typedef _TextPrintOptsDart = int Function(Pointer<Void> msg, int printUnknown,
    Pointer<Pointer<Uint8>> out, Pointer<IntPtr> len);
typedef _MillisNative = Int32 Function(Pointer<Void> msg, Pointer<Int64> out);
typedef _MillisDart = int Function(Pointer<Void> msg, Pointer<Int64> out);
typedef _FromMillisNative = Int32 Function(
    Pointer<Void> desc, Int64 ms, Pointer<Pointer<Void>> out);
typedef _FromMillisDart = int Function(
    Pointer<Void> desc, int ms, Pointer<Pointer<Void>> out);
typedef _AnyPackNative = Int32 Function(Pointer<Void> desc, Pointer<Utf8> url,
    Pointer<Void> payload, IntPtr len, Pointer<Pointer<Void>> out);
typedef _AnyPackDart = int Function(Pointer<Void> desc, Pointer<Utf8> url,
    Pointer<Void> payload, int len, Pointer<Pointer<Void>> out);
typedef _CountNative = IntPtr Function(Pointer<Void> ptr);
typedef _CountDart = int Function(Pointer<Void> ptr);
typedef _AtNative = Pointer<Void> Function(Pointer<Void> ptr, IntPtr index);
typedef _AtDart = Pointer<Void> Function(Pointer<Void> ptr, int index);
typedef _MethodStrNative = Pointer<Utf8> Function(Pointer<Void> ptr);
typedef _MethodStrDart = Pointer<Utf8> Function(Pointer<Void> ptr);
typedef _PathAtNative = Pointer<Utf8> Function(Pointer<Void> ptr, IntPtr index);
typedef _PathAtDart = Pointer<Utf8> Function(Pointer<Void> ptr, int index);
typedef _MethodIntNative = Int32 Function(Pointer<Void> ptr);
typedef _MethodIntDart = int Function(Pointer<Void> ptr);
typedef _EncodedSizeNative = Int32 Function(Pointer<Void> msg, Pointer<IntPtr> len);
typedef _EncodedSizeDart = int Function(Pointer<Void> msg, Pointer<IntPtr> len);
typedef _ServiceResolveNative = Int32 Function(
    Pointer<Void> schema,
    Pointer<Utf8> service,
    Pointer<Utf8> method,
    Pointer<Pointer<Void>> inDesc,
    Pointer<Pointer<Void>> outDesc);
typedef _ServiceResolveDart = int Function(
    Pointer<Void> schema,
    Pointer<Utf8> service,
    Pointer<Utf8> method,
    Pointer<Pointer<Void>> inDesc,
    Pointer<Pointer<Void>> outDesc);
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
        encodeDeterministic =
            lib.lookupFunction<_EncodeNative, _EncodeDart>('pbw_encode_deterministic'),
        encodeDelimited =
            lib.lookupFunction<_EncodeNative, _EncodeDart>('pbw_encode_delimited'),
        decode = lib.lookupFunction<_DecodeNative, _DecodeDart>('pbw_decode'),
        decodeDelimited = lib
            .lookupFunction<_DecodeDelimitedNative, _DecodeDelimitedDart>('pbw_decode_delimited'),
        decodeLimits = lib
            .lookupFunction<_DecodeLimitsNative, _DecodeLimitsDart>('pbw_decode_limits'),
        visit = lib.lookupFunction<_VisitNative, _VisitDart>('pbw_visit_limits'),
        jsonPrint = lib.lookupFunction<_EncodeNative, _EncodeDart>('pbw_json_print'),
        jsonParse =
            lib.lookupFunction<_JsonParseNative, _JsonParseDart>('pbw_json_parse_opts'),
        jsonParseLimits = lib.lookupFunction<_JsonParseLimitsNative,
            _JsonParseLimitsDart>('pbw_json_parse_limits'),
        textPrint = lib.lookupFunction<_EncodeNative, _EncodeDart>('pbw_text_print'),
        textPrintOpts = lib
            .lookupFunction<_TextPrintOptsNative, _TextPrintOptsDart>('pbw_text_print_opts'),
        textParse = lib.lookupFunction<_DecodeNative, _DecodeDart>('pbw_text_parse'),
        textParseLimits = lib.lookupFunction<_TextParseLimitsNative,
            _TextParseLimitsDart>('pbw_text_parse_limits'),
        messageEncodedSize = lib.lookupFunction<_EncodedSizeNative, _EncodedSizeDart>(
            'pbw_message_encoded_size'),
        messageIsInitialized =
            lib.lookupFunction<_Int32Native, _Int32Dart>('pbw_message_is_initialized'),
        anyPack = lib.lookupFunction<_AnyPackNative, _AnyPackDart>('pbw_any_pack'),
        anyTypeUrl = lib.lookupFunction<_NameNative, _NameDart>('pbw_any_type_url'),
        anyPayload = lib.lookupFunction<_ValueBytesNative, _ValueBytesDart>('pbw_any_payload'),
        timestampToMillis = lib
            .lookupFunction<_MillisNative, _MillisDart>('pbw_timestamp_to_millis'),
        durationToMillis = lib
            .lookupFunction<_MillisNative, _MillisDart>('pbw_duration_to_millis'),
        timestampFromMillis = lib
            .lookupFunction<_FromMillisNative, _FromMillisDart>('pbw_timestamp_from_millis'),
        durationFromMillis = lib
            .lookupFunction<_FromMillisNative, _FromMillisDart>('pbw_duration_from_millis'),
        fieldmaskPathCount = lib
            .lookupFunction<_CountNative, _CountDart>('pbw_fieldmask_path_count'),
        fieldmaskPathAt =
            lib.lookupFunction<_PathAtNative, _PathAtDart>('pbw_fieldmask_path_at'),
        schemaServiceCount = lib
            .lookupFunction<_CountNative, _CountDart>('pbw_schema_service_count'),
        schemaServiceAt =
            lib.lookupFunction<_AtNative, _AtDart>('pbw_schema_service_at'),
        schemaServiceFind =
            lib.lookupFunction<_SchemaFindNative, _SchemaFindDart>('pbw_schema_service_find'),
        serviceName = lib.lookupFunction<_NameNative, _NameDart>('pbw_service_name'),
        serviceMethodCount = lib
            .lookupFunction<_CountNative, _CountDart>('pbw_service_method_count'),
        serviceMethodAt =
            lib.lookupFunction<_AtNative, _AtDart>('pbw_service_method_at'),
        serviceMethodFind =
            lib.lookupFunction<_SchemaFindNative, _SchemaFindDart>('pbw_service_method_find'),
        methodName = lib.lookupFunction<_NameNative, _NameDart>('pbw_method_name'),
        methodInputType =
            lib.lookupFunction<_MethodStrNative, _MethodStrDart>('pbw_method_input_type'),
        methodOutputType =
            lib.lookupFunction<_MethodStrNative, _MethodStrDart>('pbw_method_output_type'),
        methodClientStreaming = lib
            .lookupFunction<_MethodIntNative, _MethodIntDart>('pbw_method_client_streaming'),
        methodServerStreaming = lib
            .lookupFunction<_MethodIntNative, _MethodIntDart>('pbw_method_server_streaming'),
        serviceResolve = lib
            .lookupFunction<_ServiceResolveNative, _ServiceResolveDart>('pbw_service_resolve'),
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
  final _EncodeDart encodeDeterministic;
  final _EncodeDart encodeDelimited;
  final _DecodeDart decode;
  final _DecodeDelimitedDart decodeDelimited;
  final _DecodeLimitsDart decodeLimits;
  final _VisitDart visit;
  final _EncodeDart jsonPrint;
  final _JsonParseDart jsonParse;
  final _JsonParseLimitsDart jsonParseLimits;
  final _EncodeDart textPrint;
  final _TextPrintOptsDart textPrintOpts;
  final _DecodeDart textParse;
  final _TextParseLimitsDart textParseLimits;
  final _EncodedSizeDart messageEncodedSize;
  final _Int32Dart messageIsInitialized;
  final _AnyPackDart anyPack;
  final _NameDart anyTypeUrl;
  final _ValueBytesDart anyPayload;
  final _MillisDart timestampToMillis;
  final _MillisDart durationToMillis;
  final _FromMillisDart timestampFromMillis;
  final _FromMillisDart durationFromMillis;
  final _CountDart fieldmaskPathCount;
  final _PathAtDart fieldmaskPathAt;
  final _CountDart schemaServiceCount;
  final _AtDart schemaServiceAt;
  final _FindFieldDart schemaServiceFind;
  final _NameDart serviceName;
  final _CountDart serviceMethodCount;
  final _AtDart serviceMethodAt;
  final _FindFieldDart serviceMethodFind;
  final _NameDart methodName;
  final _MethodStrDart methodInputType;
  final _MethodStrDart methodOutputType;
  final _MethodIntDart methodClientStreaming;
  final _MethodIntDart methodServerStreaming;
  final _ServiceResolveDart serviceResolve;
  final _StrerrorDart strerror;
}

PbwireLibrary get _lib => PbwireLibrary.instance;

PbwireException _statusError(int status) =>
    PbwireException(status, _lib.strerror(status).toDartString());

/// `pbw_limits`: zero means the defaults (no byte cap, `PBW_MAX_DEPTH`).
/// Bounds as `pbw_limits` expects them: zero means the default, and a negative
/// value would wrap into an unsigned C field (a cap silently disabled), so it
/// is rejected here.
void _checkLimits(int maxBytes, int maxDepth) {
  // Dart ints are signed 64-bit: the largest byte cap a size_t can take.
  if (maxBytes < 0 || maxBytes > 0x7FFFFFFFFFFFFFFF) {
    throw ArgumentError.value(maxBytes, 'maxBytes', 'must be a non-negative int');
  }
  if (maxDepth < 0 || maxDepth > 0xFFFFFFFF) {
    throw ArgumentError.value(maxDepth, 'maxDepth', 'must be a non-negative int');
  }
}

final class _Limits extends Struct {
  @Size()
  external int maxBytes;

  @UnsignedInt()
  external int maxDepth;
}

/// `pbw_json_parse_options`: skip unknown field names instead of failing.
final class _JsonOptions extends Struct {
  @Int()
  external int ignoreUnknown;
}

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
  bool _closed = false;

  void _ensureOpen() {
    if (_closed || _handle == nullptr) {
      throw PbwireException(-1, 'schema is closed');
    }
  }

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

  /// How many services the loaded files declare.
  int get serviceCount {
    _ensureOpen();
    return _lib.schemaServiceCount(_handle);
  }

  /// One service by index.
  Service serviceAt(int index) {
    _ensureOpen();
    final ptr = _lib.schemaServiceAt(_handle, index);
    if (ptr == nullptr) {
      throw PbwireException(-1, 'no service at index $index');
    }
    return Service._(this, ptr);
  }

  /// One service by fully qualified name, e.g. `pkg.Service`.
  Service service(String name) {
    _ensureOpen();
    final native = name.toNativeUtf8();
    try {
      final ptr = _lib.schemaServiceFind(_handle, native);
      if (ptr == nullptr) {
        throw PbwireException(-1, 'no such service: $name');
      }
      return Service._(this, ptr);
    } finally {
      malloc.free(native);
    }
  }

  /// Resolves a method to its input/output message types for routing: decode
  /// with the input, encode with the output.
  ({Type input, Type output}) resolveMethod(String service, String method) {
    _ensureOpen();
    final svc = service.toNativeUtf8();
    final meth = method.toNativeUtf8();
    final inDesc = calloc<Pointer<Void>>();
    final outDesc = calloc<Pointer<Void>>();
    try {
      final status = _lib.serviceResolve(_handle, svc, meth, inDesc, outDesc);
      if (status != 0) {
        throw _statusError(status);
      }
      return (input: Type._(this, inDesc.value), output: Type._(this, outDesc.value));
    } finally {
      malloc.free(svc);
      malloc.free(meth);
      calloc.free(inDesc);
      calloc.free(outDesc);
    }
  }

  /// Looks up a message by fully qualified name, e.g. `structtest.Scalars3`.
  Type message(String fullName) {
    _ensureOpen();
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
      _closed = true;
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

  void _ensureOpen() => _schema._ensureOpen();

  String get name {
    _ensureOpen();
    return _lib.descriptorName(_desc).toDartString();
  }

  /// Resolves a field once and caches it: a later set/get costs one native call.
  Field field(String name) {
    _ensureOpen();
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
      return _fields[name] = Field._(ptr, this);
    } finally {
      malloc.free(native);
    }
  }

  /// A new empty message; freed by [Message.close] (or the finalizer).
  Message newMessage() {
    _ensureOpen();
    final ptr = _lib.messageNew(_desc);
    if (ptr == nullptr) {
      throw PbwireException(-1, 'pbw_message_new: out of memory');
    }
    return Message._root(this, ptr);
  }

  /// Decodes wire bytes; unknown fields are preserved and re-encoded.
  Message decode(List<int> wire) {
    _ensureOpen();
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

  /// Decodes one length-delimited frame from the front of [wire], returning
  /// the message and how many bytes the frame occupied.
  ({Message message, int consumed}) decodeDelimited(List<int> wire) {
    _ensureOpen();
    final data = _copyIn(wire);
    final out = calloc<Pointer<Void>>();
    final consumed = malloc<IntPtr>();
    try {
      final status = _lib.decodeDelimited(_desc, data.cast(), wire.length, out, consumed);
      if (status != 0) {
        throw _statusError(status);
      }
      if (out.value == nullptr) {
        throw PbwireException(-1, 'decode: no message returned');
      }
      return (message: Message._root(this, out.value), consumed: consumed.value);
    } finally {
      if (data != nullptr) {
        malloc.free(data);
      }
      calloc.free(out);
      malloc.free(consumed);
    }
  }

  /// Decodes with explicit bounds: inputs larger than [maxBytes] fail, and
  /// nesting past [maxDepth] fails. Zero means the defaults.
  Message decodeLimits(List<int> wire, {int maxBytes = 0, int maxDepth = 0}) {
    _ensureOpen();
    _checkLimits(maxBytes, maxDepth);
    final data = _copyIn(wire);
    final out = calloc<Pointer<Void>>();
    final limits = calloc<_Limits>();
    try {
      limits.ref.maxBytes = maxBytes;
      limits.ref.maxDepth = maxDepth;
      final status =
          _lib.decodeLimits(_desc, data.cast(), wire.length, limits.cast(), out);
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
      calloc.free(limits);
    }
  }

  /// Scans [wire] one level deep without building a tree: one callback per
  /// field, the core allocating nothing. Nested messages and packed runs
  /// arrive as [VisitField.data] payloads for the caller to recurse into.
  /// Returning false stops the scan without an error.
  void visit(List<int> wire, bool Function(VisitField field) cb,
      {int maxBytes = 0, int maxDepth = 0}) {
    _ensureOpen();
    _checkLimits(maxBytes, maxDepth);
    final data = _copyIn(wire);
    final limits = calloc<_Limits>();
    final failOff = malloc<IntPtr>();
    final state = _VisitState(this, cb);
    final native = NativeCallable<_VisitFnNative>.isolateLocal(
        _visitTrampoline, exceptionalReturn: 1);
    try {
      limits.ref.maxBytes = maxBytes;
      limits.ref.maxDepth = maxDepth;
      // Save/restore, not clear: a callback may start another visit, and
      // the outer scan must still find its own state afterwards.
      final previous = _visitUser;
      _visitUser = state;
      try {
        final status = _lib.visit(_desc, data.cast(), wire.length,
            limits.cast(), native.nativeFunction, nullptr, failOff);
        if (status != 0 && !state.stopped) {
          throw _statusError(status);
        }
      } finally {
        _visitUser = previous;
      }
    } finally {
      native.close();
      if (data != nullptr) {
        malloc.free(data);
      }
      calloc.free(limits);
      malloc.free(failOff);
    }
  }

  /// Parses JSON mapping text into a new tree. Unknown fields fail unless
  /// [ignoreUnknown] is set; [maxBytes]/[maxDepth] bound the input.
  Message parseJson(String text,
      {bool ignoreUnknown = false, int maxBytes = 0, int maxDepth = 0}) {
    _ensureOpen();
    _checkLimits(maxBytes, maxDepth);
    final native = text.toNativeUtf8();
    final opts = calloc<_JsonOptions>();
    final limits = calloc<_Limits>();
    final out = calloc<Pointer<Void>>();
    try {
      opts.ref.ignoreUnknown = ignoreUnknown ? 1 : 0;
      limits.ref.maxBytes = maxBytes;
      limits.ref.maxDepth = maxDepth;
      final status = _lib.jsonParseLimits(
          _desc, native, native.length, limits.cast(), opts.cast(), out);
      if (status != 0) {
        throw _statusError(status);
      }
      if (out.value == nullptr) {
        throw PbwireException(-1, 'parse: no message returned');
      }
      return Message._root(this, out.value);
    } finally {
      malloc.free(native);
      calloc.free(opts);
      calloc.free(limits);
      calloc.free(out);
    }
  }

  /// Parses text format into a new tree; [maxBytes]/[maxDepth] bound the input.
  Message parseText(String text, {int maxBytes = 0, int maxDepth = 0}) {
    _ensureOpen();
    _checkLimits(maxBytes, maxDepth);
    final native = text.toNativeUtf8();
    final limits = calloc<_Limits>();
    final out = calloc<Pointer<Void>>();
    try {
      limits.ref.maxBytes = maxBytes;
      limits.ref.maxDepth = maxDepth;
      final status = _lib.textParseLimits(
          _desc, native.cast(), native.length, limits.cast(), out);
      if (status != 0) {
        throw _statusError(status);
      }
      if (out.value == nullptr) {
        throw PbwireException(-1, 'parse: no message returned');
      }
      return Message._root(this, out.value);
    } finally {
      malloc.free(native);
      calloc.free(limits);
      calloc.free(out);
    }
  }

  /// Packs [payload] into a new `google.protobuf.Any` tree. This type is the
  /// Any descriptor.
  Message anyPack(String typeUrl, List<int> payload) {
    _ensureOpen();
    final url = typeUrl.toNativeUtf8();
    final data = _copyIn(payload);
    final out = calloc<Pointer<Void>>();
    try {
      final status =
          _lib.anyPack(_desc, url, data.cast(), payload.length, out);
      if (status != 0) {
        throw _statusError(status);
      }
      return Message._root(this, out.value);
    } finally {
      malloc.free(url);
      if (data != nullptr) {
        malloc.free(data);
      }
      calloc.free(out);
    }
  }

  /// A new `google.protobuf.Timestamp` tree holding [ms]. This type is the
  /// Timestamp descriptor.
  Message timestampFromMillis(int ms) {
    _ensureOpen();
    final out = calloc<Pointer<Void>>();
    try {
      final status = _lib.timestampFromMillis(_desc, ms, out);
      if (status != 0) {
        throw _statusError(status);
      }
      return Message._root(this, out.value);
    } finally {
      calloc.free(out);
    }
  }

  /// A new `google.protobuf.Duration` tree holding [ms]. This type is the
  /// Duration descriptor.
  Message durationFromMillis(int ms) {
    _ensureOpen();
    final out = calloc<Pointer<Void>>();
    try {
      final status = _lib.durationFromMillis(_desc, ms, out);
      if (status != 0) {
        throw _statusError(status);
      }
      return Message._root(this, out.value);
    } finally {
      calloc.free(out);
    }
  }

  /// The message type of a MESSAGE/GROUP field, for nested access.
  Type? nestedType(Field field) {
    _ensureOpen();
    final desc = _lib.fieldMessageType(field._ptr);
    return desc == nullptr ? null : Type._(_schema, desc);
  }
}

/// A resolved field handle.
/// One scanned field, filled by [Type.visit]. [name] is empty when the
/// schema does not declare the number; [raw] carries the wire bits for
/// varint and the fixed widths; [data] carries LEN payloads and whole
/// groups (it points into the caller's wire for the callback only).
final class VisitField {
  VisitField(this.name, this.number, this.wireType, this.raw, this.data);

  final String name;
  final int number;
  final int wireType;
  final int raw;
  final Uint8List? data;
}

/* The callback the C side calls. NativeCallable.isolateLocal only accepts a
 * top-level function, so the user callback and its Type travel through this
 * field: visit is synchronous, and one isolate runs one callback at a time. */
class _VisitState {
  _VisitState(this.type, this.cb);

  final Type type;
  final bool Function(VisitField field) cb;
  bool stopped = false;
}

_VisitState? _visitUser;

int _visitTrampoline(Pointer<Void> ud, Pointer<Void> field, int number,
    int wireType, Pointer<_VisitValue> value) {
  final state = _visitUser;
  if (state == null) {
    return 1;
  }
  final v = value.ref;
  Uint8List? data;
  if (v.data != nullptr) {
    data = Uint8List.fromList(
        v.data.asTypedList(v.len)); // copied: the wire outlives the callback
  }
  final name = field == nullptr ? '' : _lib.fieldName(field).toDartString();
  if (!state.cb(VisitField(name, number, wireType, v.u64, data))) {
    state.stopped = true;
    return 1;
  }
  return 0;
}

final class Field {
  Field._(this._ptr, this._type);

  final Pointer<Void> _ptr;

  /// Keeps the Type (and through it the Schema) alive while the field is used.
  final Type _type;

  void _check() => _type._ensureOpen();

  String get name {
    _check();
    return _lib.fieldName(_ptr).toDartString();
  }

  int get number {
    _check();
    return _lib.fieldNumber(_ptr);
  }

  int get type {
    _check();
    return _lib.fieldType(_ptr);
  }

  /// 3 for repeated.
  int get label {
    _check();
    return _lib.fieldLabel(_ptr);
  }
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

  /// A root owns the arena; a view only borrows a slice of it. Closing any
  /// ancestor frees the memory a view points into, so every access checks the
  /// whole parent chain first (the Dart object alone does not keep it alive --
  /// the native tree does).
  void _check() {
    _type._ensureOpen();
    if (_handle == nullptr) {
      throw PbwireException(-1, 'message is closed');
    }
    for (Message? p = _parent; p != null; p = p._parent) {
      if (p._handle == nullptr) {
        throw PbwireException(-1, 'parent message is closed');
      }
    }
  }

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
    _check();
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
    _check();
    field._check();
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

  void clear(Object field) {
    _check();
    _lib.messageClear(_handle, _resolve(field).number);
  }

  /* ---------------------------------------------------------------- reading */

  Pointer<Void>? _value(Field field) {
    _check();
    field._check();
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

  Uint8List _encodeWith(_EncodeDart fn) {
    _check();
    final out = calloc<Pointer<Uint8>>();
    final len = malloc<IntPtr>();
    try {
      final status = fn(_handle, out, len);
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

  /// Encoded bytes (one allocation by the core, one copy into a Dart list).
  Uint8List encode() {
    _check();
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

  /// Canonical encoding: map entries sorted by key, like the reference.
  Uint8List encodeDeterministic() => _encodeWith(_lib.encodeDeterministic);

  /// Encodes into caller-owned native memory (`malloc`/`calloc`) and returns
  /// the bytes written: no allocation, `cap` is the hard limit. The core
  /// over-reserves a few bytes per field, so size it as [encodedSize] + 12
  /// when it must fit. A Dart `Uint8List` cannot be passed here: the GC may
  /// move it, so allocate with `package:ffi`'s `malloc`.
  int encodeTo(Pointer<Uint8> out, int cap) {
    _check();
    if (cap < 1) {
      throw ArgumentError.value(cap, 'cap', 'needs at least one byte');
    }
    final len = malloc<IntPtr>();
    try {
      final status =
          _lib.encodeTo(_handle, out.cast(), cap, len);
      if (status != 0) {
        throw _statusError(status);
      }
      return len.value;
    } finally {
      malloc.free(len);
    }
  }

  /// One length-delimited frame (varint length + message).
  Uint8List encodeDelimited() => _encodeWith(_lib.encodeDelimited);

  /// Protobuf JSON mapping. Unknown fields are not printed.
  String printJson() => utf8.decode(_encodeWith(_lib.jsonPrint));

  /// Protobuf text format. Unknown fields print as numeric lines when
  /// [printUnknown] is set, like the reference printer.
  String printText([bool printUnknown = false]) {
    if (!printUnknown) {
      return utf8.decode(_encodeWith(_lib.textPrint));
    }
    _check();
    final out = calloc<Pointer<Uint8>>();
    final len = malloc<IntPtr>();
    try {
      final status = _lib.textPrintOpts(_handle, 1, out, len);
      if (status != 0) {
        throw _statusError(status);
      }
      final bytes = Uint8List.fromList(out.value.asTypedList(len.value));
      _lib.free(out.value.cast());
      return utf8.decode(bytes);
    } finally {
      calloc.free(out);
      malloc.free(len);
    }
  }

  /// Exact bytes [encode] would write, without writing them.
  int encodedSize() {
    _check();
    final len = malloc<IntPtr>();
    try {
      final status = _lib.messageEncodedSize(_handle, len);
      if (status != 0) {
        throw _statusError(status);
      }
      return len.value;
    } finally {
      malloc.free(len);
    }
  }

  /// True when every proto2 `required` field is present.
  bool isInitialized() {
    _check();
    return _lib.messageIsInitialized(_handle) != 0;
  }

  /// Field 1 of a `google.protobuf.Any` tree, or null when absent.
  String? anyTypeUrl() {
    _check();
    final ptr = _lib.anyTypeUrl(_handle);
    return ptr == nullptr ? null : ptr.toDartString();
  }

  /// Field 2 of a `google.protobuf.Any` tree, or null when absent.
  Uint8List? anyPayload() {
    _check();
    final len = malloc<IntPtr>();
    try {
      final ptr = _lib.anyPayload(_handle, len);
      if (ptr == nullptr) {
        return null;
      }
      return Uint8List.fromList(ptr.cast<Uint8>().asTypedList(len.value));
    } finally {
      malloc.free(len);
    }
  }

  /// A `google.protobuf.Timestamp` tree as milliseconds.
  int timestampMillis() {
    _check();
    final out = malloc<Int64>();
    try {
      final status = _lib.timestampToMillis(_handle, out);
      if (status != 0) {
        throw _statusError(status);
      }
      return out.value;
    } finally {
      malloc.free(out);
    }
  }

  /// A `google.protobuf.Duration` tree as milliseconds.
  int durationMillis() {
    _check();
    final out = malloc<Int64>();
    try {
      final status = _lib.durationToMillis(_handle, out);
      if (status != 0) {
        throw _statusError(status);
      }
      return out.value;
    } finally {
      malloc.free(out);
    }
  }

  /// Every path of a `google.protobuf.FieldMask` tree.
  List<String> fieldMaskPaths() {
    _check();
    final n = _lib.fieldmaskPathCount(_handle);
    return List.generate(n, (i) => _lib.fieldmaskPathAt(_handle, i).toDartString());
  }

  void close() {
    if (_handle == nullptr) {
      return;
    }
    if (_owned) {
      _finalizer.detach(this);
      _lib.messageFree(_handle);
    }
    /* A view closes itself too (its parent link stays for the ancestor check). */
    _handle = nullptr;
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

  /* The value lives in the owner's arena: check it before every read. */
  Pointer<Void> get _handle {
    _owner._check();
    return _ptr;
  }

  int asInt() => _lib.valueInt(_handle);
  bool asBool() => _lib.valueBool(_handle) != 0;
  double asFloat() => _lib.valueFloat(_handle);
  double asDouble() => _lib.valueDouble(_handle);

  String? asString() {
    final ptr = _lib.valueString(_handle);
    return ptr == nullptr ? null : ptr.toDartString();
  }

  Uint8List? asBytes() {
    final len = malloc<IntPtr>();
    try {
      final ptr = _lib.valueBytes(_handle, len);
      return ptr == nullptr ? null : Uint8List.fromList(ptr.cast<Uint8>().asTypedList(len.value));
    } finally {
      malloc.free(len);
    }
  }

  Message? asMessage() {
    final ptr = _lib.valueMessage(_handle);
    final type = _type;
    if (ptr == nullptr || type == null) {
      return null;
    }
    return Message._view(type, ptr, _owner);
  }
}

/* -------------------------------------------------------------- services */

/// A service from a [Schema]: routing metadata, not RPC.
final class Service {
  Service._(this._schema, this._ptr);

  final Schema _schema;

  /// Keeps the Schema alive while the service is used.
  final Pointer<Void> _ptr;

  void _check() => _schema._ensureOpen();

  String get name {
    _check();
    return _lib.serviceName(_ptr).toDartString();
  }

  int get methodCount {
    _check();
    return _lib.serviceMethodCount(_ptr);
  }

  Method methodAt(int index) {
    _check();
    final ptr = _lib.serviceMethodAt(_ptr, index);
    if (ptr == nullptr) {
      throw PbwireException(-1, 'no method at index $index');
    }
    return Method._(_schema, ptr);
  }

  Method method(String name) {
    _check();
    final native = name.toNativeUtf8();
    try {
      final ptr = _lib.serviceMethodFind(_ptr, native);
      if (ptr == nullptr) {
        throw PbwireException(-1, 'no such method: $name');
      }
      return Method._(_schema, ptr);
    } finally {
      malloc.free(native);
    }
  }
}

/// One service method: fully qualified input/output type names for routing.
final class Method {
  Method._(this._schema, this._ptr);

  final Schema _schema;
  final Pointer<Void> _ptr;

  void _check() => _schema._ensureOpen();

  String get name {
    _check();
    return _lib.methodName(_ptr).toDartString();
  }

  /// Fully qualified input message name, without the leading dot.
  String get inputType {
    _check();
    return _lib.methodInputType(_ptr).toDartString();
  }

  /// Fully qualified output message name, without the leading dot.
  String get outputType {
    _check();
    return _lib.methodOutputType(_ptr).toDartString();
  }

  bool get clientStreaming {
    _check();
    return _lib.methodClientStreaming(_ptr) != 0;
  }

  bool get serverStreaming {
    _check();
    return _lib.methodServerStreaming(_ptr) != 0;
  }
}

/* ------------------------------------------------- the native layout reader */

/// pbw_struct_kind_t, in the C header's order: one row per field, saying where
/// that field lives in the generated `<Message>Layout`.
enum PbwKind {
  int32,
  int64,
  uint32,
  uint64,
  boolean,
  float32,
  float64,
  string_,
  bytes,
  enumeration,
  message,
}

/// The core's inline byte string (`pbw_bytes_t`): pointer plus length. Used as
/// a Struct member so a `bytes` field needs no pointer indirection.
final class PbwBytes extends Struct {
  external Pointer<Uint8> data;

  @IntPtr()
  external int len;
}

/// One field row of a message's flat layout. The generated `*LayoutRows`
/// constants are lists of these; [PbwireLayout] hands them to the C reader,
/// which walks the Dart Struct in place. A row with [proname] set is a sub-
/// message and names its proto full name, so the child must be registered first.
final class PbwField {
  const PbwField({
    required this.number,
    required this.kind,
    required this.offset,
    this.repeated = false,
    this.has = 0,
    this.count = 0,
    this.child = '',
    this.oneof = false,
    this.cs = 0,
    this.required = false,
    this.type = 0,
    this.wire = 0,
    this.packed = false,
  });

  /// Proto field number.
  final int number;

  /// Slot kind: scalar, string, bytes, enum or sub-message.
  final PbwKind kind;

  /// Byte offset of the value slot (`size_t` pointer slot for repeated fields).
  final int offset;

  final bool repeated;

  /// Offset of the 4-byte presence flag (proto3 `optional` / proto2 optional).
  final int has;

  /// Offset of the `size_t` element count of a repeated field.
  final int count;

  /// Proto full name of the sub-message this row points at.
  final String child;

  final bool oneof;

  /// Offset of the oneof's shared case word.
  final int cs;

  final bool required;

  /// `pbw_type` (0 asks the C codec to read it from the schema).
  final int type;

  /// Wire type of one element (0 asks the codec).
  final int wire;

  /// Repeated numerics packed on the wire.
  final bool packed;
}

/// The row as C reads it: every field 64-bit so the Dart mirror has no padding
/// rules to get wrong, then the child's proto full name.
final class PbwDartRow extends Struct {
  @Uint64()
  external int number;

  @Uint64()
  external int kind;

  @Uint64()
  external int repeated;

  @Uint64()
  external int offset;

  @Uint64()
  external int has;

  @Uint64()
  external int count;

  @Uint64()
  external int type;

  @Uint64()
  external int wire;

  @Uint64()
  external int packed;

  @Uint64()
  external int required;

  @Uint64()
  external int oneof;

  @Uint64()
  external int cs;

  external Pointer<Utf8> child;
}

typedef _DartSchemaLoadNative = Int32 Function(
    Pointer<Void> data, IntPtr len, Pointer<Pointer<Void>> out);
typedef _DartSchemaLoadDart = int Function(
    Pointer<Void> data, int len, Pointer<Pointer<Void>> out);
typedef _DartSchemaFreeNative = Void Function(Pointer<Void> schema);
typedef _DartSchemaFreeDart = void Function(Pointer<Void> schema);
typedef _DartTypeNewNative = Int32 Function(Pointer<Void> schema,
    Pointer<Utf8> message, Pointer<PbwDartRow> rows, IntPtr nrows, IntPtr size,
    Pointer<Pointer<Void>> out);
typedef _DartTypeNewDart = int Function(Pointer<Void> schema,
    Pointer<Utf8> message, Pointer<PbwDartRow> rows, int nrows, int size,
    Pointer<Pointer<Void>> out);
typedef _DartEncodeNative = Int32 Function(Pointer<Void> type, Pointer<Void> base,
    Pointer<Pointer<Uint8>> out, Pointer<IntPtr> outLen);
typedef _DartEncodeDart = int Function(Pointer<Void> type, Pointer<Void> base,
    Pointer<Pointer<Uint8>> out, Pointer<IntPtr> outLen);
typedef _DartDecodeNative = Int32 Function(Pointer<Void> type,
    Pointer<Uint8> wire, IntPtr wireLen, Pointer<Void> base,
    Pointer<Uint8> arena, IntPtr arenaLen);
typedef _DartDecodeDart = int Function(Pointer<Void> type,
    Pointer<Uint8> wire, int wireLen, Pointer<Void> base,
    Pointer<Uint8> arena, int arenaLen);

/// The `bindings/dart/native` shim library: the C-side layout reader that
/// walks a `dart:ffi` Struct in place with the C binding's `pbw_struct` codec.
final class PbwireShimLibrary {
  PbwireShimLibrary._(DynamicLibrary lib)
      : schemaLoad = lib.lookupFunction<_DartSchemaLoadNative, _DartSchemaLoadDart>(
            'pbw_dart_schema_load'),
        schemaFree = lib.lookupFunction<_DartSchemaFreeNative, _DartSchemaFreeDart>(
            'pbw_dart_schema_free'),
        typeNew = lib.lookupFunction<_DartTypeNewNative, _DartTypeNewDart>(
            'pbw_dart_type_new'),
        encode = lib.lookupFunction<_DartEncodeNative, _DartEncodeDart>(
            'pbw_dart_encode'),
        decode = lib.lookupFunction<_DartDecodeNative, _DartDecodeDart>(
            'pbw_dart_decode');

  static PbwireShimLibrary? _instance;

  /// The process-wide shim; built by `bindings/dart/build.sh`.
  static PbwireShimLibrary get instance =>
      _instance ??= PbwireShimLibrary._(DynamicLibrary.open(path()));

  /// `PBWIRE_DART_LIB`, else `build/libpbwire_dart.{dylib,so}` under the
  /// package directory found by walking up from the working directory or the
  /// entry script.
  static String path() {
    final env = Platform.environment['PBWIRE_DART_LIB'];
    if (env != null && env.isNotEmpty) {
      return env;
    }
    for (final start in [Directory.current.path, File.fromUri(Platform.script).parent.path]) {
      var dir = Directory(start).absolute;
      while (true) {
        for (final rel in ['build', 'bindings/dart/build']) {
          for (final suffix in ['dylib', 'so']) {
            final candidate = File('${dir.path}/$rel/libpbwire_dart.$suffix');
            if (candidate.existsSync()) {
              return candidate.path;
            }
          }
        }
        final parent = dir.parent;
        if (parent.path == dir.path) {
          break;
        }
        dir = parent;
      }
    }
    throw PbwireException(-1,
        'no Dart shim library found (set PBWIRE_DART_LIB; run bindings/dart/build.sh)');
  }

  final _DartSchemaLoadDart schemaLoad;
  final _DartSchemaFreeDart schemaFree;
  final _DartTypeNewDart typeNew;
  final _DartEncodeDart encode;
  final _DartDecodeDart decode;
}

PbwireShimLibrary get _dartLib => PbwireShimLibrary.instance;

PbwireException _dartStatus(int status) {
  final lib = PbwireLibrary.instance;
  return PbwireException(status, lib.strerror(status).toDartString());
}

/// The layout side's schema: the shim library links its own copy of the core,
/// so it loads the descriptor set itself (a tree [Schema] handle stays on the
/// core library). Messages register in dependency order: children first.
final class PbwLayoutSchema {
  PbwLayoutSchema.fromBytes(Uint8List bytes)
      : _names = Arena() {
    final data = malloc<Uint8>(bytes.isEmpty ? 1 : bytes.length);
    final out = calloc<Pointer<Void>>();
    try {
      data.asTypedList(bytes.length).setAll(0, bytes);
      final status = _dartLib.schemaLoad(data.cast(), bytes.length, out);
      if (status != 0) {
        throw _dartStatus(status);
      }
      _handle = out.value;
    } finally {
      malloc.free(data);
      calloc.free(out);
    }
  }

  static PbwLayoutSchema load(String path) =>
      PbwLayoutSchema.fromBytes(File(path).readAsBytesSync());

  Pointer<Void> _handle = nullptr;
  final Arena _names;
  final Map<String, PbwType> _types = {};

  /// Compiles one message layout: [size] is `sizeOf<MessageLayout>()`, [rows]
  /// the generated offset table. Sub-message rows must already be registered.
  PbwType type(String message, List<PbwField> rows, int size) {
    if (_handle == nullptr) {
      throw PbwireException(-1, 'PbwLayoutSchema is closed');
    }
    final existing = _types[message];
    if (existing != null) {
      return existing;
    }
    final native = _names.allocate<Utf8>(message.length + 1);
    native.cast<Uint8>().asTypedList(message.length + 1).setAll(0, message.codeUnits);
    final raw = _names.allocate<PbwDartRow>(
        (rows.length == 0 ? 1 : rows.length) * sizeOf<PbwDartRow>());
    final childNames = <Pointer<Utf8>>[];
    for (var i = 0; i < rows.length; i++) {
      final row = rows[i];
      raw[i]
        ..number = row.number
        ..kind = row.kind.index
        ..repeated = row.repeated ? 1 : 0
        ..offset = row.offset
        ..has = row.has
        ..count = row.count
        ..type = row.type
        ..wire = row.wire
        ..packed = row.packed ? 1 : 0
        ..required = row.required ? 1 : 0
        ..oneof = row.oneof ? 1 : 0
        ..cs = row.cs;
      if (row.child.isNotEmpty) {
        final childName = _names.allocate<Utf8>(row.child.length + 1);
        childName
            .cast<Uint8>()
            .asTypedList(row.child.length + 1)
            .setAll(0, row.child.codeUnits);
        childNames.add(childName);
        raw[i].child = childName;
      }
    }
    final out = calloc<Pointer<Void>>();
    try {
      final status = _dartLib.typeNew(_handle, native, raw, rows.length, size, out);
      if (status != 0) {
        throw _dartStatus(status);
      }
      final type = PbwType._(this, out.value, size, message);
      _types[message] = type;
      return type;
    } finally {
      calloc.free(out);
    }
  }

  void close() {
    if (_handle == nullptr) {
      return;
    }
    for (final type in _types.values) {
      type._release();
    }
    _types.clear();
    _dartLib.schemaFree(_handle);
    _names.releaseAll();
    _handle = nullptr;
  }
}

/// One compiled message layout: rows, C codec and the Dart Struct size.
final class PbwType {
  PbwType._(this.schema, this._handle, this.size, this.message)
      : _out = calloc<Pointer<Uint8>>(),
        _outLen = calloc<IntPtr>();

  final PbwLayoutSchema schema;
  final String message;

  /// `sizeOf<Layout>()`: the bytes of one flat image.
  final int size;

  Pointer<Void> _handle;
  final Pointer<Pointer<Uint8>> _out;
  final Pointer<IntPtr> _outLen;

  void _release() {
    calloc.free(_out);
    calloc.free(_outLen);
  }
}

/// The call-owned arena for one decode: the wire copy and the C bump arena
/// live in one block. Every string/bytes/sub-message pointer a decode writes
/// points into it, so release it in a `finally` after the last read; reuse it
/// across calls (each decode resets it) for the steady state the C core's arena
/// is built for.
final class PbwArena {
  PbwArena(int wireLength, {int extra = 0})
      : wireLength = wireLength,
        _arenaOffset = (wireLength + 7) & ~7,
        _arenaBytes = wireLength * 4 + 65536 + extra,
        _block = malloc<Uint8>(((wireLength + 7) & ~7) + wireLength * 4 + 65536 + extra);

  final int wireLength;
  final int _arenaOffset;
  final int _arenaBytes;
  final Pointer<Uint8> _block;

  /// Where `decodeInto` copies the wire bytes (8-byte aligned).
  Pointer<Uint8> get wire => _block;

  /// The C bump arena inside the same block (`pbw_struct_arena_new_mem`).
  Pointer<Uint8> get arena => _block + _arenaOffset;

  int get arenaBytes => _arenaBytes;

  void release() => malloc.free(_block);
}

/// One FFI call per op over the generated layout image.
final class PbwireLayout {
  PbwireLayout._();

  /// Encodes one flat image into wire bytes. The returned list is a fresh
  /// `Uint8List`: the C buffer is reused by the next call of the same type.
  static Uint8List encode<T extends Struct>(Pointer<T> message, PbwType type) {
    final status = _dartLib.encode(type._handle, message.cast(), type._out, type._outLen);
    if (status != 0) {
      throw _dartStatus(status);
    }
    final len = type._outLen.value;
    if (len == 0) {
      return Uint8List(0);
    }
    final bytes = type._out.value.cast<Uint8>();
    return Uint8List.fromList(bytes.asTypedList(len));
  }

  /// Decodes [wire] into a fresh flat image at [into], one FFI call. The image
  /// borrows [arena]: strings, bytes and sub-messages point into it, so read
  /// [into] before releasing the arena.
  static void decodeInto<T extends Struct>(
      Uint8List wire, Pointer<T> into, PbwType type, PbwArena arena) {
    if (wire.length > arena.wireLength) {
      throw PbwireException(-1,
          'wire is ${wire.length} bytes, the arena has ${arena.wireLength}');
    }
    arena.wire.asTypedList(wire.length).setAll(0, wire);
    final status = _dartLib.decode(
        type._handle, arena.wire, wire.length, into.cast(), arena.arena, arena.arenaBytes);
    if (status == 2) {
      throw PbwireException(status, 'decode arena too small: widen PbwArena(extra:)');
    }
    if (status != 0) {
      throw _dartStatus(status);
    }
  }
}
