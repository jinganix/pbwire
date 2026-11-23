// The C# binding: P/Invoke over the C ABI in include/pbwire.h. A
// FileDescriptorSet (protoc --descriptor_set_out) becomes the schema at run
// time; there is no generated code and no C glue. The wire format lives in the
// C core, so a C# message and a C message are the same bytes by construction.
//
// Ownership: a Schema and a root Message own native memory and Dispose() frees
// it; a nested message is a view of its parent and never frees anything. One
// message belongs to one thread, like the C core.
//
// The library is PBWIRE_LIB, else build/libpbwire.{dylib,so} next to the
// checkout (`make shared` builds it).

using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;

namespace Pbwire;

/// <summary>Anything the C core refused, plus I/O failures from <see cref="Schema.FromFile"/>.</summary>
public sealed class PbwireException : Exception
{
    public PbwireException(int status, string message) : base(message) => Status = status;

    /// <summary>The pbw_status value, or -1 for an I/O failure.</summary>
    public int Status { get; }
}

internal static class Native
{
    private const string Lib = "pbwire";
    private const string ShimLib = "pbwire_cs_shim";

    static Native() =>
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, _, _) => name switch
        {
            Lib => NativeLibrary.Load(LibraryPath("pbwire")),
            ShimLib => NativeLibrary.Load(ShimPath()),
            _ => IntPtr.Zero,
        });

    /// <summary>PBWIRE_LIB, else build/libpbwire.{dylib,so} found above the working directory.</summary>
    internal static string LibraryPath() => LibraryPath("pbwire");

    /// <summary>The library beside PBWIRE_LIB, else build/lib&lt;name&gt;.{dylib,so} above the working directory.</summary>
    private static string LibraryPath(string stem)
    {
        var env = Environment.GetEnvironmentVariable("PBWIRE_LIB");
        if (!string.IsNullOrEmpty(env))
        {
            if (stem == "pbwire")
            {
                return env;
            }
            // The resolver's own resolution: the shim sits beside libpbwire.
            var dir = Path.GetDirectoryName(Path.GetFullPath(env));
            foreach (var suffix in new[] { "dylib", "so" })
            {
                var beside = Path.Combine(dir ?? ".", $"lib{stem}.{suffix}");
                if (File.Exists(beside))
                {
                    return beside;
                }
            }
        }
        for (var dir = new DirectoryInfo(Directory.GetCurrentDirectory()); dir != null; dir = dir.Parent)
        {
            foreach (var suffix in new[] { "dylib", "so" })
            {
                var candidate = Path.Combine(dir.FullName, "build", $"lib{stem}.{suffix}");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }
        if (stem == "pbwire")
        {
            throw new PbwireException(-1, "no shared library found (set PBWIRE_LIB; run `make shared`)");
        }
        throw new PbwireException(-1, $"no {stem} library found (run `make csharp-shim-native`)");
    }

    /// <summary>The C-side layout reader: beside PBWIRE_LIB, else build/.</summary>
    internal static string ShimPath() => LibraryPath(ShimLib);

    internal static string Text(IntPtr ptr) =>
        ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;

    internal static Exception Error(int status) =>
        new PbwireException(status, Text(pbw_strerror(status)));

    internal static void Check(int status)
    {
        if (status != 0)
        {
            throw Error(status);
        }
    }

    internal static IntPtr NotNull(IntPtr ptr, string what) =>
        ptr == IntPtr.Zero ? throw new PbwireException(-1, what) : ptr;

    /* schema */
    [DllImport(Lib)] internal static extern IntPtr pbw_schema_error(IntPtr schema);
    [DllImport(Lib)] internal static extern void pbw_schema_free(IntPtr schema);
    [DllImport(Lib)] internal static extern IntPtr pbw_schema_find(IntPtr schema, string name);
    [DllImport(Lib)]
    internal static extern int pbw_descriptor_set_load(IntPtr data, nuint len, out IntPtr schema);
    [DllImport(Lib)] internal static extern IntPtr pbw_descriptor_name(IntPtr desc);
    [DllImport(Lib)] internal static extern IntPtr pbw_descriptor_find_field_name(IntPtr desc, string name);
    [DllImport(Lib)] internal static extern IntPtr pbw_field_name(IntPtr field);
    [DllImport(Lib)] internal static extern uint pbw_field_number(IntPtr field);
    [DllImport(Lib)] internal static extern int pbw_field_type(IntPtr field);
    [DllImport(Lib)] internal static extern int pbw_field_label(IntPtr field);
    [DllImport(Lib)] internal static extern IntPtr pbw_field_message_type(IntPtr field);

    /* message */
    [DllImport(Lib)] internal static extern IntPtr pbw_message_new(IntPtr desc);
    [DllImport(Lib)] internal static extern void pbw_message_free(IntPtr msg);
    [DllImport(Lib)] internal static extern IntPtr pbw_message_get_field(IntPtr msg, IntPtr field);
    [DllImport(Lib)] internal static extern int pbw_message_set_field(IntPtr msg, IntPtr field, IntPtr value);
    [DllImport(Lib)] internal static extern IntPtr pbw_message_set_message(IntPtr msg, uint number);
    [DllImport(Lib)] internal static extern void pbw_message_clear(IntPtr msg, uint number);

    /* value */
    [DllImport(Lib)] internal static extern IntPtr pbw_value_new_int(long value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_new_bool(int value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_new_float(float value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_new_double(double value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_new_bytes(IntPtr data, nuint len);
    [DllImport(Lib, CharSet = CharSet.Ansi)]
    internal static extern IntPtr pbw_value_new_string([MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    [DllImport(Lib)] internal static extern void pbw_value_free(IntPtr value);
    [DllImport(Lib)] internal static extern long pbw_value_get_int(IntPtr value);
    [DllImport(Lib)] internal static extern int pbw_value_get_bool(IntPtr value);
    [DllImport(Lib)] internal static extern float pbw_value_get_float(IntPtr value);
    [DllImport(Lib)] internal static extern double pbw_value_get_double(IntPtr value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_get_bytes(IntPtr value, out nuint len);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_get_string(IntPtr value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_get_message(IntPtr value);
    [DllImport(Lib)] internal static extern nuint pbw_value_count(IntPtr value);
    [DllImport(Lib)] internal static extern IntPtr pbw_value_at(IntPtr value, nuint index);

    /* wire */
    [DllImport(Lib)] internal static extern int pbw_encode(IntPtr msg, out IntPtr data, out nuint len);
    [DllImport(Lib)]
    internal static extern int pbw_decode(IntPtr desc, IntPtr buf, nuint len, out IntPtr msg);
    [DllImport(Lib)] internal static extern int pbw_encode_deterministic(IntPtr msg, out IntPtr data, out nuint len);
    [DllImport(Lib)] internal static extern int pbw_encode_delimited(IntPtr msg, out IntPtr data, out nuint len);
    [DllImport(Lib)]
    internal static extern int pbw_decode_delimited(IntPtr desc, IntPtr buf, nuint len, out IntPtr msg, out nuint consumed);
    [DllImport(Lib)]
    internal static extern int pbw_decode_limits(IntPtr desc, IntPtr buf, nuint len, ref PbwLimits limits, out IntPtr msg);
    [DllImport(Lib)] internal static extern int pbw_json_print(IntPtr msg, out IntPtr data, out nuint len);
    [DllImport(Lib)]
    internal static extern int pbw_json_parse_opts(IntPtr desc, IntPtr text, nuint len, ref PbwJsonOptions opts, out IntPtr msg);
    [DllImport(Lib)]
    internal static extern int pbw_json_parse_limits(IntPtr desc, IntPtr text, nuint len, ref PbwLimits limits, ref PbwJsonOptions opts, out IntPtr msg);
    [DllImport(Lib)] internal static extern int pbw_text_print(IntPtr msg, out IntPtr data, out nuint len);
    [DllImport(Lib)]
    internal static extern int pbw_text_print_opts(IntPtr msg, int printUnknown, out IntPtr data, out nuint len);
    [DllImport(Lib)]
    internal static extern int pbw_text_parse(IntPtr desc, IntPtr text, nuint len, out IntPtr msg);
    [DllImport(Lib)]
    internal static extern int pbw_text_parse_limits(IntPtr desc, IntPtr text, nuint len, ref PbwLimits limits, out IntPtr msg);
    [DllImport(Lib)] internal static extern int pbw_message_encoded_size(IntPtr msg, out nuint len);
    [DllImport(Lib)] internal static extern int pbw_message_is_initialized(IntPtr msg);

    /* well-known types */
    [DllImport(Lib)]
    internal static extern int pbw_any_pack(IntPtr desc, string typeUrl, IntPtr payload, nuint len, out IntPtr msg);
    [DllImport(Lib)] internal static extern IntPtr pbw_any_type_url(IntPtr msg);
    [DllImport(Lib)] internal static extern IntPtr pbw_any_payload(IntPtr msg, out nuint len);
    [DllImport(Lib)] internal static extern int pbw_timestamp_to_millis(IntPtr msg, out long ms);
    [DllImport(Lib)] internal static extern int pbw_duration_to_millis(IntPtr msg, out long ms);
    [DllImport(Lib)] internal static extern int pbw_timestamp_from_millis(IntPtr desc, long ms, out IntPtr msg);
    [DllImport(Lib)] internal static extern int pbw_duration_from_millis(IntPtr desc, long ms, out IntPtr msg);
    [DllImport(Lib)] internal static extern nuint pbw_fieldmask_path_count(IntPtr msg);
    [DllImport(Lib)] internal static extern IntPtr pbw_fieldmask_path_at(IntPtr msg, nuint index);

    /* services */
    [DllImport(Lib)] internal static extern nuint pbw_schema_service_count(IntPtr schema);
    [DllImport(Lib)] internal static extern IntPtr pbw_schema_service_at(IntPtr schema, nuint index);
    [DllImport(Lib)] internal static extern IntPtr pbw_schema_service_find(IntPtr schema, string name);
    [DllImport(Lib)] internal static extern IntPtr pbw_service_name(IntPtr svc);
    [DllImport(Lib)] internal static extern nuint pbw_service_method_count(IntPtr svc);
    [DllImport(Lib)] internal static extern IntPtr pbw_service_method_at(IntPtr svc, nuint index);
    [DllImport(Lib)] internal static extern IntPtr pbw_service_method_find(IntPtr svc, string name);
    [DllImport(Lib)] internal static extern IntPtr pbw_method_name(IntPtr method);
    [DllImport(Lib)] internal static extern IntPtr pbw_method_input_type(IntPtr method);
    [DllImport(Lib)] internal static extern IntPtr pbw_method_output_type(IntPtr method);
    [DllImport(Lib)] internal static extern int pbw_method_client_streaming(IntPtr method);
    [DllImport(Lib)] internal static extern int pbw_method_server_streaming(IntPtr method);
    [DllImport(Lib)]
    internal static extern int pbw_service_resolve(IntPtr schema, string service, string method, out IntPtr inDesc, out IntPtr outDesc);

    /* single-level scan: one callback per field, no tree, no allocation */
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int PbwVisitFn(IntPtr ud, IntPtr field, uint number, uint wireType, ref PbwVisitValue value);
    [DllImport(Lib)]
    internal static extern int pbw_visit(IntPtr desc, IntPtr buf, nuint len, PbwVisitFn cb, IntPtr ud, out nuint failOff);
    [DllImport(Lib)] internal static extern IntPtr pbw_strerror(int status);
    [DllImport(Lib)] internal static extern void pbw_free(IntPtr ptr);

    /* the flat-image reader (bindings/csharp/native/pbw_cs_shim.c) */
    [DllImport(ShimLib)] internal static extern int pbw_cs_shim_schema_load(IntPtr data, nuint len, out IntPtr schema);
    [DllImport(ShimLib)] internal static extern void pbw_cs_shim_schema_free(IntPtr schema);
    [DllImport(ShimLib, CharSet = CharSet.Ansi)]
    internal static extern int pbw_cs_shim_type_new(IntPtr schema, [MarshalAs(UnmanagedType.LPUTF8Str)] string message,
        IntPtr rows, nuint nrows, nuint size, out IntPtr type);
    [DllImport(ShimLib)]
    internal static extern int pbw_cs_shim_encode(IntPtr type, IntPtr image, out IntPtr data, out nuint len);
    [DllImport(ShimLib)]
    internal static extern int pbw_cs_shim_decode(IntPtr type, IntPtr wire, nuint wireLen, IntPtr image,
        IntPtr arena, nuint arenaLen);
}

/// <summary>Bounds for decoding: zero fields mean the defaults (no byte cap, PBW_MAX_DEPTH nesting).</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PbwLimits
{
    public nuint MaxBytes;
    public uint MaxDepth;
}

/// <summary>JSON parse options: skip unknown field names instead of failing.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PbwJsonOptions
{
    public int IgnoreUnknown;
}

/// <summary>One scanned field: raw wire content, no allocation, no value tree.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PbwVisitValue
{
    public ulong Raw;
    public IntPtr Data;
    public nuint Len;
}

/// <summary>A loaded FileDescriptorSet: the schema every <see cref="MessageType"/> comes from.</summary>
public sealed class Schema : IDisposable
{
    private IntPtr _handle;

    private Schema(IntPtr handle) => _handle = handle;

    /// <summary>Loads a descriptor set written by <c>protoc --descriptor_set_out</c>.</summary>
    public static Schema FromFile(string path) => FromBytes(File.ReadAllBytes(path));

    /// <summary>Loads descriptor-set bytes; use <c>--include_imports</c> so every type resolves.</summary>
    public static unsafe Schema FromBytes(byte[] bytes)
    {
        fixed (byte* data = bytes)
        {
            var status = Native.pbw_descriptor_set_load((IntPtr)data, (nuint)bytes.Length, out var handle);
            if (status != 0)
            {
                var why = handle == IntPtr.Zero ? string.Empty : Native.Text(Native.pbw_schema_error(handle));
                if (handle != IntPtr.Zero)
                {
                    Native.pbw_schema_free(handle);
                }
                throw new PbwireException(status, why.Length == 0 ? Native.Text(Native.pbw_strerror(status)) : why);
            }
            return new Schema(Native.NotNull(handle, "descriptor set: no schema returned"));
        }
    }

    /// <summary>Looks up a message by fully qualified name, e.g. <c>structtest.Scalars3</c>.</summary>
    public MessageType Message(string fullName)
    {
        var desc = Native.pbw_schema_find(Handle, fullName);
        return desc == IntPtr.Zero
            ? throw new PbwireException(-1, $"no such message type: {fullName}")
            : new MessageType(this, desc);
    }

    /// <summary>How many services the loaded files declare.</summary>
    public int ServiceCount()
    {
        _ = Handle;
        return (int)Native.pbw_schema_service_count(_handle);
    }

    /// <summary>One service by index.</summary>
    public Service ServiceAt(int index)
    {
        _ = Handle;
        var ptr = Native.pbw_schema_service_at(_handle, (nuint)index);
        return ptr == IntPtr.Zero
            ? throw new PbwireException(-1, $"no service at index {index}")
            : new Service(this, ptr);
    }

    /// <summary>One service by fully qualified name, e.g. <c>pkg.Service</c>.</summary>
    public Service Service(string name)
    {
        var ptr = Native.pbw_schema_service_find(Handle, name);
        return ptr == IntPtr.Zero
            ? throw new PbwireException(-1, $"no such service: {name}")
            : new Service(this, ptr);
    }

    /// <summary>Resolves a method to its input/output message types for routing: decode with the input, encode with the output.</summary>
    public (MessageType Input, MessageType Output) ResolveMethod(string service, string method)
    {
        Native.Check(Native.pbw_service_resolve(Handle, service, method, out var inDesc, out var outDesc));
        return (new MessageType(this, Native.NotNull(inDesc, "resolve: no input type")),
                new MessageType(this, Native.NotNull(outDesc, "resolve: no output type")));
    }

    internal IntPtr Handle =>
        _handle != IntPtr.Zero ? _handle : throw new ObjectDisposedException(nameof(Schema));

    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            Native.pbw_schema_free(_handle);
            _handle = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }

    // Fallback for a caller that never disposes: freeing native memory touches
    // no managed state, so the finalizer thread can do it. Message views hold
    // their parent alive, so a schema outlives every message that still needs
    // its descriptors.
    ~Schema() => Dispose();
}

/// <summary>A message type from a <see cref="Schema"/>.</summary>
public sealed class MessageType
{
    private readonly Schema _schema;
    private readonly IntPtr _desc;
    private readonly Dictionary<string, Field> _fields = new();
    private readonly object _fieldsLock = new();

    internal MessageType(Schema schema, IntPtr desc)
    {
        _schema = schema;
        _desc = desc;
    }

    internal IntPtr Desc => _desc;

    /// <summary>A disposed Schema frees the descriptors this type points into.</summary>
    internal void EnsureOpen() => _ = _schema.Handle;

    public string Name
    {
        get
        {
            EnsureOpen();
            return Native.Text(Native.pbw_descriptor_name(_desc));
        }
    }

    /// <summary>Resolves a field once and caches it: a later set/get costs one native call.
    /// Lock-guarded like <see cref="LayoutSource"/>: a singleton MessageType is
    /// shared across first-burst request threads, so concurrent cold
    /// <c>Field</c> calls must not race on the <c>Dictionary</c> (concurrent
    /// read+write on <c>Dictionary</c> is undefined, not just a lost update).
    /// The native lookup has no managed reentrancy, so it runs inside the lock
    /// and single-construction semantics are kept.
    /// </summary>
    public Field Field(string name)
    {
        EnsureOpen();
        lock (_fieldsLock)
        {
            if (_fields.TryGetValue(name, out var cached))
            {
                return cached;
            }
            var ptr = Native.pbw_descriptor_find_field_name(_desc, name);
            if (ptr == IntPtr.Zero)
            {
                throw new PbwireException(-1, $"{Name}: no such field: {name}");
            }
            var field = new Field(this, ptr);
            _fields[name] = field;
            return field;
        }
    }

    public Message NewMessage()
    {
        EnsureOpen();
        return Message.Root(this, Native.NotNull(Native.pbw_message_new(_desc), "pbw_message_new: out of memory"));
    }

    /// <summary>Decodes wire bytes; unknown fields are preserved and re-encoded.</summary>
    public unsafe Message Decode(ReadOnlySpan<byte> wire)
    {
        EnsureOpen();
        fixed (byte* data = wire)
        {
            Native.Check(Native.pbw_decode(_desc, (IntPtr)data, (nuint)wire.Length, out var msg));
            return Message.Root(this, Native.NotNull(msg, "decode: no message returned"));
        }
    }

    /// <summary>Decodes one length-delimited frame from the front of the wire, returning the message and how many bytes the frame occupied.</summary>
    public unsafe (Message Message, int Consumed) DecodeDelimited(ReadOnlySpan<byte> wire)
    {
        EnsureOpen();
        fixed (byte* data = wire)
        {
            Native.Check(Native.pbw_decode_delimited(_desc, (IntPtr)data, (nuint)wire.Length, out var msg, out var consumed));
            if ((ulong)consumed > int.MaxValue)
            {
                Native.pbw_message_free(msg);
                throw new PbwireException(-1, "pbwire: delimited frame exceeds the span limit");
            }
            return (Message.Root(this, Native.NotNull(msg, "decode: no message returned")), (int)consumed);
        }
    }

    /// <summary>Decodes with explicit bounds: inputs larger than <paramref name="maxBytes"/> fail, and nesting past <paramref name="maxDepth"/> fails. Zero means the defaults.</summary>
    public unsafe Message DecodeLimits(ReadOnlySpan<byte> wire, ulong maxBytes = 0, uint maxDepth = 0)
    {
        EnsureOpen();
        var limits = new PbwLimits { MaxBytes = (nuint)maxBytes, MaxDepth = maxDepth };
        fixed (byte* data = wire)
        {
            Native.Check(Native.pbw_decode_limits(_desc, (IntPtr)data, (nuint)wire.Length, ref limits, out var msg));
            return Message.Root(this, Native.NotNull(msg, "decode: no message returned"));
        }
    }

    /// <summary>Parses JSON mapping text into a new tree. Unknown fields fail unless <paramref name="ignoreUnknown"/> is set.</summary>
    /// <summary>Parses JSON mapping text into a new tree. <paramref name="maxBytes"/> and <paramref name="maxDepth"/> bound the input (0 = the defaults).</summary>
    public unsafe Message ParseJson(string text, bool ignoreUnknown = false, ulong maxBytes = 0, uint maxDepth = 0)
    {
        EnsureOpen();
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        var opts = new PbwJsonOptions { IgnoreUnknown = ignoreUnknown ? 1 : 0 };
        var limits = new PbwLimits { MaxBytes = (nuint)maxBytes, MaxDepth = maxDepth };
        fixed (byte* data = bytes)
        {
            Native.Check(maxBytes == 0 && maxDepth == 0
                ? Native.pbw_json_parse_opts(_desc, (IntPtr)data, (nuint)bytes.Length, ref opts, out var msg0)
                : Native.pbw_json_parse_limits(_desc, (IntPtr)data, (nuint)bytes.Length, ref limits, ref opts, out msg0));
            return Message.Root(this, Native.NotNull(msg0, "parse: no message returned"));
        }
    }

    /// <summary>Parses text format into a new tree; <paramref name="maxBytes"/> and <paramref name="maxDepth"/> bound the input (0 = the defaults).</summary>
    public unsafe Message ParseText(string text, ulong maxBytes = 0, uint maxDepth = 0)
    {
        EnsureOpen();
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        var limits = new PbwLimits { MaxBytes = (nuint)maxBytes, MaxDepth = maxDepth };
        fixed (byte* data = bytes)
        {
            Native.Check(maxBytes == 0 && maxDepth == 0
                ? Native.pbw_text_parse(_desc, (IntPtr)data, (nuint)bytes.Length, out var msg0)
                : Native.pbw_text_parse_limits(_desc, (IntPtr)data, (nuint)bytes.Length, ref limits, out msg0));
            return Message.Root(this, Native.NotNull(msg0, "parse: no message returned"));
        }
    }

    /// <summary>Packs <paramref name="payload"/> into a new <c>google.protobuf.Any</c> tree. This type is the Any descriptor.</summary>
    public unsafe Message AnyPack(string typeUrl, ReadOnlySpan<byte> payload)
    {
        EnsureOpen();
        fixed (byte* data = payload)
        {
            Native.Check(Native.pbw_any_pack(_desc, typeUrl, (IntPtr)data, (nuint)payload.Length, out var msg));
            return Message.Root(this, Native.NotNull(msg, "pack: no message returned"));
        }
    }

    /// <summary>A new <c>google.protobuf.Timestamp</c> tree holding <paramref name="ms"/>. This type is the Timestamp descriptor.</summary>
    public Message TimestampFromMillis(long ms)
    {
        EnsureOpen();
        Native.Check(Native.pbw_timestamp_from_millis(_desc, ms, out var msg));
        return Message.Root(this, Native.NotNull(msg, "pack: no message returned"));
    }

    /// <summary>A new <c>google.protobuf.Duration</c> tree holding <paramref name="ms"/>. This type is the Duration descriptor.</summary>
    public Message DurationFromMillis(long ms)
    {
        EnsureOpen();
        Native.Check(Native.pbw_duration_from_millis(_desc, ms, out var msg));
        return Message.Root(this, Native.NotNull(msg, "pack: no message returned"));
    }

    /// <summary>Single-level scan of the wire without building a message tree: one callback per field, no allocation. Nested messages and packed runs arrive as byte payloads for the caller to recurse into. Returning false stops the scan without an exception.</summary>
    public unsafe void Visit(ReadOnlySpan<byte> wire, Func<VisitField, bool> callback)
    {
        EnsureOpen();
        var handle = GCHandle.Alloc(callback);
        bool aborted = false;
        Exception? thrown = null;
        try
        {
            Native.PbwVisitFn fn = (IntPtr ud, IntPtr field, uint number, uint wireType, ref PbwVisitValue value) =>
            {
                /* A managed exception must not unwind through the native
                 * frame: stop the scan and rethrow after it returns. */
                try
                {
                    var cb = (Func<VisitField, bool>)GCHandle.FromIntPtr(ud).Target!;
                    byte[]? bytes = null;
                    if (value.Data != IntPtr.Zero)
                    {
                        if (value.Len > int.MaxValue)
                        {
                            aborted = true;
                            return 1; // one LEN payload cannot exceed a span
                        }
                        bytes = new ReadOnlySpan<byte>((void*)value.Data, (int)value.Len).ToArray();
                    }
                    var type = field == IntPtr.Zero ? null : NestedTypeOf(field);
                    if (cb(new VisitField(field, number, wireType, value.Raw, bytes, type)))
                    {
                        return 0;
                    }
                    aborted = true;
                    return 1;
                }
                catch (Exception e)
                {
                    thrown = e;
                    aborted = true;
                    return 1;
                }
            };
            fixed (byte* data = wire)
            {
                var status = Native.pbw_visit(_desc, (IntPtr)data, (nuint)wire.Length, fn, GCHandle.ToIntPtr(handle), out _);
                // A callback stop is not a failure: the core reports the
                // callback's status, so only a real failure throws here.
                if (status != 0 && !aborted)
                {
                    Native.Check(status);
                }
            }
            GC.KeepAlive(fn);
        }
        finally
        {
            handle.Free();
        }
        if (thrown != null)
        {
            ExceptionDispatchInfo.Capture(thrown).Throw();
        }
    }

    private MessageType? NestedTypeOf(IntPtr field)
    {
        var desc = Native.pbw_field_message_type(field);
        return desc == IntPtr.Zero ? null : new MessageType(_schema, desc);
    }

    /// <summary>The message type of a MESSAGE/GROUP field, for nested access.</summary>
    public MessageType? NestedType(Field field)
    {
        EnsureOpen();
        var desc = Native.pbw_field_message_type(field.Ptr);
        return desc == IntPtr.Zero ? null : new MessageType(_schema, desc);
    }
}

/// <summary>A resolved field handle.</summary>
public readonly struct Field
{
    private readonly MessageType _owner;

    internal Field(MessageType owner, IntPtr ptr)
    {
        _owner = owner;
        Ptr = ptr;
    }

    internal IntPtr Ptr { get; }

    public string Name
    {
        get
        {
            _owner.EnsureOpen();
            return Native.Text(Native.pbw_field_name(Ptr));
        }
    }

    public uint Number
    {
        get
        {
            _owner.EnsureOpen();
            return Native.pbw_field_number(Ptr);
        }
    }

    /// <summary>pbw_type: 5 = int32, 11 = message, ...</summary>
    public int Type
    {
        get
        {
            _owner.EnsureOpen();
            return Native.pbw_field_type(Ptr);
        }
    }

    /// <summary>3 = repeated.</summary>
    public int Label
    {
        get
        {
            _owner.EnsureOpen();
            return Native.pbw_field_label(Ptr);
        }
    }
}

/// <summary>One dynamic message. A root owns its tree; a nested message borrows it.</summary>
public sealed class Message : IDisposable
{
    private readonly MessageType _type;
    private readonly Message? _parent;
    private readonly bool _owned;
    private IntPtr _handle;

    private Message(MessageType type, IntPtr handle, bool owned, Message? parent)
    {
        _type = type;
        _handle = handle;
        _owned = owned;
        _parent = parent;
    }

    internal static Message Root(MessageType type, IntPtr handle) => new(type, handle, true, null);

    /// <summary>A nested message: the parent's tree stays alive through it.</summary>
    internal static Message View(MessageType type, IntPtr handle, Message parent) =>
        new(type, handle, false, parent);

    public MessageType Type => _type;

    /// <summary>Keeps the owning message reachable; null for a root.</summary>
    internal Message? Parent => _parent;

    internal IntPtr Handle
    {
        get
        {
            _type.EnsureOpen();
            // A view lives inside its root's arena: disposing any ancestor
            // frees the memory it points into.
            for (var p = _parent; p != null; p = p._parent)
            {
                if (p._handle == IntPtr.Zero)
                {
                    throw new ObjectDisposedException(nameof(Message), "parent message is disposed");
                }
            }
            return _handle != IntPtr.Zero
                ? _handle
                : throw new ObjectDisposedException(nameof(Message));
        }
    }

    /* ------------------------------------------------------------ setting */

    /// <summary>Any integer or enum field; the value is stored as raw 64-bit bits.</summary>
    public Message Set(string field, long value) => Store(_type.Field(field), Native.pbw_value_new_int(value));

    public Message Set(string field, bool value) => Store(_type.Field(field), Native.pbw_value_new_bool(value ? 1 : 0));

    public Message Set(string field, float value) => Store(_type.Field(field), Native.pbw_value_new_float(value));

    public Message Set(string field, double value) => Store(_type.Field(field), Native.pbw_value_new_double(value));

    public Message Set(string field, string value) =>
        Store(_type.Field(field),
              value == null ? throw new ArgumentNullException(nameof(value), "pbwire: string value is null")
                            : Native.pbw_value_new_string(value));

    public unsafe Message Set(string field, ReadOnlySpan<byte> value)
    {
        fixed (byte* data = value)
        {
            var ptr = Native.pbw_value_new_bytes((IntPtr)data, (nuint)value.Length);
            if (ptr == IntPtr.Zero && value.Length != 0)
            {
                throw new PbwireException(-1, $"set {field}: out of memory");
            }
            return Store(_type.Field(field), ptr);
        }
    }

    /// <summary>Sets the same field handle again, skipping the name lookup.</summary>
    public Message Set(Field field, long value) => Store(field, Native.pbw_value_new_int(value));

    public Message Set(Field field, string value) => Store(field, Native.pbw_value_new_string(value));

    /// <summary>Nested message for a MESSAGE field, created in place (or returned as it is).</summary>
    public Message SetMessage(string field) => SetMessage(_type.Field(field));

    public Message SetMessage(Field field)
    {
        var ptr = Native.pbw_message_set_message(Handle, field.Number);
        if (ptr == IntPtr.Zero)
        {
            throw new PbwireException(-1, $"set {field.Name}: not a message field");
        }
        var type = _type.NestedType(field) ?? throw new PbwireException(-1, $"set {field.Name}: no nested type");
        return View(type, ptr, this);
    }

    /// <summary>Appends to a repeated field or replaces a singular one.</summary>
    private Message Store(Field field, IntPtr value)
    {
        if (value == IntPtr.Zero)
        {
            throw new PbwireException(-1, $"set {field.Name}: out of memory");
        }
        var status = Native.pbw_message_set_field(Handle, field.Ptr, value);
        if (status != 0)
        {
            /* A rejected value stays owned by the caller. */
            Native.pbw_value_free(value);
            throw new PbwireException(status, $"set {field.Name}: {Native.Text(Native.pbw_strerror(status))}");
        }
        return this;
    }

    public void Clear(string field) => Native.pbw_message_clear(Handle, _type.Field(field).Number);

    /* ------------------------------------------------------------ reading */

    private IntPtr Value(Field field) => Native.pbw_message_get_field(Handle, field.Ptr);

    public bool Has(string field) => Value(_type.Field(field)) != IntPtr.Zero;

    public long GetLong(string field)
    {
        var value = Value(_type.Field(field));
        return value == IntPtr.Zero ? 0 : Native.pbw_value_get_int(value);
    }

    public long GetLong(Field field)
    {
        var value = Value(field);
        return value == IntPtr.Zero ? 0 : Native.pbw_value_get_int(value);
    }

    public int GetInt(string field) => (int)GetLong(field);

    public bool GetBool(string field)
    {
        var value = Value(_type.Field(field));
        return value != IntPtr.Zero && Native.pbw_value_get_bool(value) != 0;
    }

    public float GetFloat(string field)
    {
        var value = Value(_type.Field(field));
        return value == IntPtr.Zero ? 0f : Native.pbw_value_get_float(value);
    }

    public double GetDouble(string field)
    {
        var value = Value(_type.Field(field));
        return value == IntPtr.Zero ? 0d : Native.pbw_value_get_double(value);
    }

    public string? GetString(string field)
    {
        var value = Value(_type.Field(field));
        return value == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(Native.pbw_value_get_string(value));
    }

    public unsafe byte[]? GetBytes(string field)
    {
        var value = Value(_type.Field(field));
        if (value == IntPtr.Zero)
        {
            return null;
        }
        var ptr = Native.pbw_value_get_bytes(value, out var len);
        if (ptr == IntPtr.Zero)
        {
            return null;
        }
        if ((ulong)len > int.MaxValue)
        {
            throw new PbwireException(-1, "pbwire: bytes payload exceeds the span limit");
        }
        return new ReadOnlySpan<byte>((void*)ptr, (int)len).ToArray();
    }

    public Message? GetMessage(string field) => GetMessage(_type.Field(field));

    public Message? GetMessage(Field field)
    {
        var value = Value(field);
        if (value == IntPtr.Zero)
        {
            return null;
        }
        var ptr = Native.pbw_value_get_message(value);
        var type = _type.NestedType(field);
        return ptr == IntPtr.Zero || type == null ? null : View(type, ptr, this);
    }

    /// <summary>Items in a repeated field, or 0 when the field is absent.</summary>
    public int Count(string field)
    {
        var value = Value(_type.Field(field));
        if (value == IntPtr.Zero)
        {
            return 0;
        }
        var count = Native.pbw_value_count(value);
        if (count > int.MaxValue)
        {
            throw new PbwireException(-1, "pbwire: repeated field exceeds the int limit");
        }
        return (int)count;
    }

    /// <summary>One item of a repeated field.</summary>
    public Value? At(string field, int index)
    {
        var resolved = _type.Field(field);
        var value = Value(resolved);
        if (value == IntPtr.Zero)
        {
            return null;
        }
        var item = Native.pbw_value_at(value, (nuint)index);
        return item == IntPtr.Zero ? null : new Value(item, this, _type.NestedType(resolved));
    }

    /* --------------------------------------------------------------- wire */

    /// <summary>Encoded bytes (one allocation by the core, one copy into a managed array).</summary>
    public unsafe byte[] Encode()
    {
        Native.Check(Native.pbw_encode(Handle, out var data, out var len));
        try
        {
            return Take(data, len);
        }
        finally
        {
            Native.pbw_free(data);
        }
    }

    private static unsafe byte[] Take(IntPtr data, nuint len)
    {
        if ((ulong)len > int.MaxValue)
        {
            throw new PbwireException(-1, "pbwire: payload exceeds the span limit");
        }
        return new ReadOnlySpan<byte>((void*)data, (int)len).ToArray();
    }

    /// <summary>Canonical encoding: map entries sorted by key, like the reference.</summary>
    public unsafe byte[] EncodeDeterministic()
    {
        Native.Check(Native.pbw_encode_deterministic(Handle, out var data, out var len));
        try
        {
            return Take(data, len);
        }
        finally
        {
            Native.pbw_free(data);
        }
    }

    /// <summary>One length-delimited frame (varint length + message).</summary>
    public unsafe byte[] EncodeDelimited()
    {
        Native.Check(Native.pbw_encode_delimited(Handle, out var data, out var len));
        try
        {
            return Take(data, len);
        }
        finally
        {
            Native.pbw_free(data);
        }
    }

    /// <summary>Protobuf JSON mapping. Unknown fields are not printed.</summary>
    public unsafe string PrintJson()
    {
        Native.Check(Native.pbw_json_print(Handle, out var data, out var len));
        try
        {
            return System.Text.Encoding.UTF8.GetString((byte*)data, (int)len);
        }
        finally
        {
            Native.pbw_free(data);
        }
    }

    /// <summary>Protobuf text format. Unknown fields print as numeric lines when <paramref name="printUnknown"/> is set, like the reference printer.</summary>
    public unsafe string PrintText(bool printUnknown = false)
    {
        var status = Native.pbw_text_print_opts(Handle, printUnknown ? 1 : 0,
            out var data, out var len);
        Native.Check(status);
        try
        {
            return System.Text.Encoding.UTF8.GetString((byte*)data, (int)len);
        }
        finally
        {
            Native.pbw_free(data);
        }
    }

    /// <summary>Exact bytes <see cref="Encode"/> would write, without writing them.</summary>
    public ulong EncodedSize()
    {
        Native.Check(Native.pbw_message_encoded_size(Handle, out var len));
        return (ulong)len;
    }

    /// <summary>True when every proto2 <c>required</c> field is present.</summary>
    public bool IsInitialized() => Native.pbw_message_is_initialized(Handle) != 0;

    /// <summary>Field 1 of a <c>google.protobuf.Any</c> tree, or null when absent.</summary>
    public string? AnyTypeUrl()
    {
        var ptr = Native.pbw_any_type_url(Handle);
        return ptr == IntPtr.Zero ? null : Native.Text(ptr);
    }

    /// <summary>Field 2 of a <c>google.protobuf.Any</c> tree, or null when absent.</summary>
    public unsafe byte[]? AnyPayload()
    {
        var ptr = Native.pbw_any_payload(Handle, out var len);
        return ptr == IntPtr.Zero ? null : Take(ptr, len);
    }

    /// <summary>A <c>google.protobuf.Timestamp</c> tree as milliseconds.</summary>
    public long TimestampMillis()
    {
        Native.Check(Native.pbw_timestamp_to_millis(Handle, out var ms));
        return ms;
    }

    /// <summary>A <c>google.protobuf.Duration</c> tree as milliseconds.</summary>
    public long DurationMillis()
    {
        Native.Check(Native.pbw_duration_to_millis(Handle, out var ms));
        return ms;
    }

    /// <summary>Every path of a <c>google.protobuf.FieldMask</c> tree.</summary>
    public string[] FieldMaskPaths()
    {
        var n = (int)Native.pbw_fieldmask_path_count(Handle);
        var paths = new string[n];
        for (var i = 0; i < n; i++)
        {
            paths[i] = Native.Text(Native.pbw_fieldmask_path_at(Handle, (nuint)i));
        }
        return paths;
    }

    public void Dispose()
    {
        if (_handle != IntPtr.Zero && _owned)
        {
            Native.pbw_message_free(_handle);
        }
        _handle = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }

    // Same fallback as Schema: a root tree a caller forgot to dispose is
    // released when it becomes unreachable. A view (owned == false) frees
    // nothing; its _parent reference keeps the root alive until the view dies.
    ~Message() => Dispose();
}

/// <summary>A field value; only repeated items need it, hence the tiny surface.</summary>
public sealed class Value
{
    private readonly IntPtr _ptr;
    private readonly Message _owner;
    private readonly MessageType? _type;

    internal Value(IntPtr ptr, Message owner, MessageType? type)
    {
        _ptr = ptr;
        _owner = owner;
        _type = type;
    }

    /* The value lives in the owner's arena: prove it is still live first. */
    private IntPtr Ptr
    {
        get
        {
            _ = _owner.Handle;
            return _ptr;
        }
    }

    public long AsLong() => Native.pbw_value_get_int(Ptr);

    public int AsInt() => (int)AsLong();

    public bool AsBool() => Native.pbw_value_get_bool(Ptr) != 0;

    public float AsFloat() => Native.pbw_value_get_float(Ptr);

    public double AsDouble() => Native.pbw_value_get_double(Ptr);

    public string? AsString() => Marshal.PtrToStringUTF8(Native.pbw_value_get_string(Ptr));

    public unsafe byte[]? AsBytes()
    {
        var ptr = Native.pbw_value_get_bytes(Ptr, out var len);
        if (ptr == IntPtr.Zero)
        {
            return null;
        }
        if ((ulong)len > int.MaxValue)
        {
            throw new PbwireException(-1, "pbwire: bytes payload exceeds the span limit");
        }
        return new ReadOnlySpan<byte>((void*)ptr, (int)len).ToArray();
    }

    public Message? AsMessage()
    {
        var ptr = Native.pbw_value_get_message(Ptr);
        if (ptr == IntPtr.Zero || _type == null)
        {
            return null;
        }
        return Message.View(_type, ptr, _owner);
    }
}

/// <summary>One scanned field: raw wire content, no allocation, no value tree. <see cref="FieldPtr"/> is zero when the number is unknown to the schema.</summary>
public sealed class VisitField
{
    internal VisitField(IntPtr fieldPtr, uint number, uint wireType, ulong raw, byte[]? data, MessageType? type)
    {
        FieldPtr = fieldPtr;
        Number = number;
        WireType = wireType;
        Raw = raw;
        Data = data;
        Type = type;
    }

    /// <summary>Schema field handle, or zero when the number is unknown.</summary>
    public IntPtr FieldPtr { get; }

    public uint Number { get; }

    public uint WireType { get; }

    /// <summary>VARINT/32BIT/64BIT raw wire bits (no zigzag or sign extension). Unused for the length-delimited shapes.</summary>
    public ulong Raw { get; }

    /// <summary>LEN payload bytes (nested message, packed run, string), or the whole raw field for groups. Null for the scalar shapes.</summary>
    public byte[]? Data { get; }

    /// <summary>The message type of a MESSAGE/GROUP field payload, for recursing with <c>Visit</c>. Null otherwise.</summary>
    public MessageType? Type { get; }
}

/// <summary>A service from a <see cref="Schema"/>: routing metadata, not RPC.</summary>
public sealed class Service
{
    private readonly Schema _schema;
    private readonly IntPtr _ptr;

    internal Service(Schema schema, IntPtr ptr)
    {
        _schema = schema;
        _ptr = ptr;
    }

    public string Name
    {
        get
        {
            _ = _schema.Handle;
            return Native.Text(Native.pbw_service_name(_ptr));
        }
    }

    public int MethodCount()
    {
        _ = _schema.Handle;
        return (int)Native.pbw_service_method_count(_ptr);
    }

    public Method MethodAt(int index)
    {
        _ = _schema.Handle;
        var ptr = Native.pbw_service_method_at(_ptr, (nuint)index);
        return ptr == IntPtr.Zero
            ? throw new PbwireException(-1, $"no method at index {index}")
            : new Method(_schema, ptr);
    }

    public Method Method(string name)
    {
        _ = _schema.Handle;
        var ptr = Native.pbw_service_method_find(_ptr, name);
        return ptr == IntPtr.Zero
            ? throw new PbwireException(-1, $"no such method: {name}")
            : new Method(_schema, ptr);
    }
}

/// <summary>One service method: fully qualified input/output type names for routing.</summary>
public sealed class Method
{
    private readonly Schema _schema;
    private readonly IntPtr _ptr;

    internal Method(Schema schema, IntPtr ptr)
    {
        _schema = schema;
        _ptr = ptr;
    }

    private void EnsureOpen() => _ = _schema.Handle;

    public string Name
    {
        get
        {
            EnsureOpen();
            return Native.Text(Native.pbw_method_name(_ptr));
        }
    }

    /// <summary>Fully qualified input message name, without the leading dot.</summary>
    public string InputType
    {
        get
        {
            EnsureOpen();
            return Native.Text(Native.pbw_method_input_type(_ptr));
        }
    }

    /// <summary>Fully qualified output message name, without the leading dot.</summary>
    public string OutputType
    {
        get
        {
            EnsureOpen();
            return Native.Text(Native.pbw_method_output_type(_ptr));
        }
    }

    public bool ClientStreaming
    {
        get
        {
            EnsureOpen();
            return Native.pbw_method_client_streaming(_ptr) != 0;
        }
    }

    public bool ServerStreaming
    {
        get
        {
            EnsureOpen();
            return Native.pbw_method_server_streaming(_ptr) != 0;
        }
    }
}

/// <summary>A generated model's shipped tree converter: how a native object fills a <see cref="Message"/> and reads one back.</summary>
public interface INativeModel
{
    /// <summary>Writes every present field of the native object into the tree.</summary>
    void WriteTo(Message msg);

    /// <summary>Reads every field of the tree into the native object.</summary>
    void ReadFrom(Message msg);
}

/// <summary>One row of a flat slot layout: where a field's storage sits in the canonical flat image.</summary>
public readonly record struct LayoutSlot(string Name, int Offset, int Size, int Presence);

/// <summary>
/// The flat slot layout a native reader would consume: scalar slots at byte
/// offsets, one pointer slot per reference-typed field (an indirection, since
/// <c>string</c>/<c>bytes</c>/submessages/repeated fields are managed
/// references), and one presence bit per proto3 optional. Codegen emits one per
/// message and <see cref="Verify"/> checks it against the descriptor; the walk
/// itself stays in the generated model (see <c>pbench/README.md</c>).
/// </summary>
public sealed class Layout
{
    private readonly List<LayoutSlot> _slots = new();
    private readonly Dictionary<string, LayoutSlot> _byName = new(StringComparer.Ordinal);

    public Layout(string name, int bytes, int presenceBits)
    {
        Name = name;
        Bytes = bytes;
        PresenceBits = presenceBits;
    }

    /// <summary>The fully qualified message this table describes.</summary>
    public string Name { get; }

    /// <summary>Size of the flat image in bytes.</summary>
    public int Bytes { get; }

    /// <summary>Number of presence bits the image carries.</summary>
    public int PresenceBits { get; }

    /// <summary>Every row, in slot order.</summary>
    public IReadOnlyList<LayoutSlot> Slots => _slots;

    /// <summary>A plain scalar slot at <paramref name="offset"/>.</summary>
    public Layout FieldAt(string name, int offset, int size) => Add(new LayoutSlot(name, offset, size, -1));

    /// <summary>A proto3 optional: presence bit <paramref name="bit"/>, storage at <paramref name="offset"/>.</summary>
    public Layout Has(string name, int bit, int offset) => Add(new LayoutSlot(name, offset, 0, bit));

    /// <summary>A reference-typed slot: a pointer indirection, not flat data.</summary>
    public Layout Ref(string name, int offset, int size) => Add(new LayoutSlot(name, offset, size, -1));

    /// <summary>The row for a field, or throws when the table does not describe it.</summary>
    public LayoutSlot Slot(string name) =>
        _byName.TryGetValue(name, out var slot) ? slot : throw new PbwireException(-1, $"{Name}: no layout row for {name}");

    /// <summary>
    /// Checks the table against the descriptor: every row names a real field,
    /// offsets stay inside the image and never overlap backwards, presence bits
    /// are distinct and inside the bitmap. A wrong table fails here instead of
    /// producing bad bytes.
    /// </summary>
    public Layout Verify(MessageType type)
    {
        if (type.Name != Name)
        {
            throw new PbwireException(-1, $"layout {Name} does not describe {type.Name}");
        }
        if (_slots.Count == 0 && (Bytes != 0 || PresenceBits != 0))
        {
            throw new PbwireException(-1, $"{Name}: empty layout with a non-zero image");
        }
        var bits = new HashSet<int>();
        var prevEnd = 0;
        foreach (var slot in _slots)
        {
            type.Field(slot.Name); // throws when the descriptor has no such field
            if (slot.Offset < 0 || slot.Offset >= Bytes || slot.Offset < prevEnd)
            {
                throw new PbwireException(-1, $"{Name}.{slot.Name}: offset {slot.Offset} outside [0, {Bytes}) or overlapping");
            }
            if (slot.Size < 0 || slot.Offset + slot.Size > Bytes)
            {
                throw new PbwireException(-1, $"{Name}.{slot.Name}: {slot.Size} bytes at {slot.Offset} run past {Bytes}");
            }
            prevEnd = slot.Offset + Math.Max(slot.Size, 1);
            if (slot.Presence >= 0)
            {
                if (slot.Presence >= PresenceBits || !bits.Add(slot.Presence))
                {
                    throw new PbwireException(-1, $"{Name}.{slot.Name}: presence bit {slot.Presence} is out of range or reused");
                }
            }
        }
        if (bits.Count != PresenceBits)
        {
            throw new PbwireException(-1, $"{Name}: {PresenceBits} presence bits declared, {bits.Count} used");
        }
        return this;
    }

    private Layout Add(LayoutSlot slot)
    {
        if (_byName.ContainsKey(slot.Name))
        {
            throw new PbwireException(-1, $"{Name}: duplicate layout row for {slot.Name}");
        }
        _byName[slot.Name] = slot;
        _slots.Add(slot);
        return this;
    }
}

/// <summary>
/// The native-object entry over a generated model: one call per op. Encode
/// allocates the tree once, lets the model's shipped converter cross once per
/// present field, and writes the bytes in one call; decode is one decode call
/// plus the model's own load.
/// </summary>
public static class Run
{
    /// <summary>Native object in, wire bytes out.</summary>
    public static byte[] Encode(object native, MessageType type)
    {
        if (native is not INativeModel model)
        {
            throw new PbwireException(-1, $"{type.Name}: not a generated model: {native.GetType().Name}");
        }
        using var msg = type.NewMessage();
        model.WriteTo(msg);
        return msg.Encode();
    }

    /// <summary>Wire bytes in, native object out.</summary>
    public static T Decode<T>(byte[] wire, MessageType type) where T : INativeModel, new()
    {
        using var msg = type.Decode(wire);
        var native = new T();
        native.ReadFrom(msg);
        return native;
    }
}

/* ------------------------------------------------------- flat image reader */

/// <summary>
/// One row of a message's flat image: <see cref="Offset"/> is where the value
/// (or, for a reference-shaped field, the pointer) sits, <see cref="Has"/> the
/// 4-byte presence word (0 = none), <see cref="Count"/> the 8-byte repeated
/// count (0 = singular), <see cref="Case"/> the 4-byte oneof case word (0 =
/// none) and <see cref="Child"/> the proto full name of a sub-message. The
/// generated <c>&lt;Message&gt;Layout</c> emits these; a native reader compiles
/// them into a C layout codec.
/// </summary>
public readonly record struct LayoutRow(
    uint Number,
    FlatKind Kind,
    int Offset,
    uint Type,
    int Wire = 0,
    bool Repeated = false,
    int Has = 0,
    int Count = 0,
    bool Packed = false,
    bool Required = false,
    bool Oneof = false,
    int Case = 0,
    string? Child = null);

/// <summary>The value shapes the C layout codec understands (pbw_struct_kind_t).
/// Named FlatKind so `[StructLayout(LayoutKind.Sequential)]` keeps working.</summary>
public enum FlatKind
{
    Int32 = 0,
    Int64 = 1,
    Uint32 = 2,
    Uint64 = 3,
    Bool = 4,
    Float = 5,
    Double = 6,
    String = 7,
    Bytes = 8,
    Enum = 9,
    Message = 10,
}

/// <summary>One message's flat image: its byte size and one row per field.</summary>
public sealed class LayoutType
{
    public LayoutType(string name, int size, LayoutRow[] rows)
    {
        Name = name;
        Size = size;
        Rows = rows;
    }

    /// <summary>The fully qualified message this image describes.</summary>
    public string Name { get; }

    /// <summary>Bytes of one image (the stride of a repeated sub-message array).</summary>
    public int Size { get; }

    /// <summary>One row per field, in slot order.</summary>
    public LayoutRow[] Rows { get; }
}

/// <summary>
/// A file's layouts: the descriptor set the shim loads for itself, the message
/// table it resolves sub-message rows from, and one compiled reader per
/// message. Generated code passes its file's instance.
/// </summary>
public sealed class LayoutSource
{
    private readonly Dictionary<string, LayoutType> _types = new(StringComparer.Ordinal);
    private readonly Dictionary<string, LayoutPlan> _plans = new(StringComparer.Ordinal);
    private readonly Func<byte[]> _descriptors;
    private readonly object _lock = new();
    private IntPtr _schema;

    public LayoutSource(string key, Func<byte[]> descriptors, LayoutType[] types)
    {
        Key = key;
        _descriptors = descriptors;
        foreach (var type in types)
        {
            _types[type.Name] = type;
        }
    }

    public string Key { get; }

    internal LayoutType? Find(string name) => _types.TryGetValue(name, out var type) ? type : null;

    /// <summary>The compiled reader for a message; children are compiled first.</summary>
    public LayoutPlan Plan(LayoutType type)
    {
        lock (_lock)
        {
            if (_plans.TryGetValue(type.Name, out var plan))
            {
                return plan;
            }
            if (_schema == IntPtr.Zero)
            {
                _schema = LoadSchema();
            }

            foreach (var row in type.Rows)
            {
                if (row.Child is { Length: > 0 } child)
                {
                    var childType = Find(child)
                        ?? throw new PbwireException(-1, $"{type.Name}: sub-message {child} has no layout in {Key}");
                    Plan(childType); // registers the child's rows and image size
                }
            }
            var native = MarshalRows(type);
            try
            {
                Native.Check(Native.pbw_cs_shim_type_new(_schema, type.Name, native.Rows,
                    (nuint)type.Rows.Length, (nuint)type.Size, out var compiled));
                plan = new LayoutPlan(compiled, type);
            }
            finally
            {
                Marshal.FreeHGlobal(native.Rows);
                if (native.Names != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(native.Names);
                }
            }
            _plans[type.Name] = plan;
            return plan;
        }
    }

    /// <summary>Loads the file's descriptor set into the shim (once): it owns its own schema copy.</summary>
    private unsafe IntPtr LoadSchema()
    {
        IntPtr schema;
        var bytes = _descriptors();
        fixed (byte* data = bytes)
        {
            Native.Check(Native.pbw_cs_shim_schema_load((IntPtr)data, (nuint)bytes.Length, out schema));
        }
        if (schema == IntPtr.Zero)
        {
            throw new PbwireException(-1, $"{Key}: descriptor set produced no schema");
        }
        return schema;
    }

    // The C row mirror: 12 eight-byte facts plus the child-name pointer (13
    // words), and the NUL-terminated names themselves in a second block. Both
    // stay alive until pbw_cs_shim_type_new has copied them.
    private static unsafe (IntPtr Rows, IntPtr Names) MarshalRows(LayoutType type)
    {
        const int Word = 8;
        const int RowBytes = 13 * Word;
        var rows = Marshal.AllocHGlobal(Math.Max(type.Rows.Length, 1) * RowBytes);
        var names = IntPtr.Zero;
        var namesLen = 0;
        foreach (var row in type.Rows)
        {
            namesLen += (row.Child?.Length ?? 0) + 1;
        }
        if (namesLen > 0)
        {
            names = Marshal.AllocHGlobal(namesLen + 1);
        }
        var nameAt = names;
        var nameUsed = 0;
        for (var i = 0; i < type.Rows.Length; i++)
        {
            var row = type.Rows[i];
            var p = (byte*)rows + i * RowBytes;
            *(ulong*)(p + 0) = row.Number;
            *(ulong*)(p + 8) = (ulong)row.Kind;
            *(ulong*)(p + 16) = row.Repeated ? 1UL : 0UL;
            *(ulong*)(p + 24) = (ulong)row.Offset;
            *(ulong*)(p + 32) = (ulong)row.Has;
            *(ulong*)(p + 40) = (ulong)row.Count;
            *(ulong*)(p + 48) = row.Type;
            *(ulong*)(p + 56) = (ulong)row.Wire;
            *(ulong*)(p + 64) = row.Packed ? 1UL : 0UL;
            *(ulong*)(p + 72) = row.Required ? 1UL : 0UL;
            *(ulong*)(p + 80) = row.Oneof ? 1UL : 0UL;
            *(ulong*)(p + 88) = (ulong)row.Case;
            *(IntPtr*)(p + 96) = IntPtr.Zero;
            if (row.Child is { Length: > 0 } child)
            {
                var dst = nameAt + nameUsed;
                var span = new Span<byte>((void*)dst, child.Length + 1);
                System.Text.Encoding.UTF8.GetBytes(child, span);
                span[child.Length] = 0;
                *(IntPtr*)(p + 96) = dst;
                nameUsed += child.Length + 1;
            }
        }
        return (rows, names);
    }

    /// <summary>Releases the shim-side schema and every compiled reader.</summary>
    public void Dispose()
    {
        lock (_lock)
        {
            _plans.Clear();
            if (_schema != IntPtr.Zero)
            {
                Native.pbw_cs_shim_schema_free(_schema);
                _schema = IntPtr.Zero;
            }
        }
        GC.SuppressFinalize(this);
    }

    ~LayoutSource() => Dispose();
}

/// <summary>One compiled native reader: encode and decode are each one call.</summary>
public sealed unsafe class LayoutPlan
{
    private readonly IntPtr _handle;

    internal LayoutPlan(IntPtr handle, LayoutType type)
    {
        _handle = handle;
        Type = type;
    }

    public LayoutType Type { get; }

    /// <summary>The compiled reader for a generated layout, cached by its file's <see cref="LayoutSource"/>.</summary>
    public static LayoutPlan Compile(LayoutSource source, LayoutType type) => source.Plan(type);

    /// <summary>Flat image in, wire bytes out (one copy out of the reused C buffer).</summary>
    public byte[] Encode(byte* image)
    {
        Native.Check(Native.pbw_cs_shim_encode(_handle, (IntPtr)image, out var data, out var len));
        if ((ulong)len > int.MaxValue)
        {
            throw new PbwireException(-1, "pbwire: payload exceeds the span limit");
        }
        return new ReadOnlySpan<byte>((void*)data, (int)len).ToArray();
    }

    /// <summary>Wire bytes in, flat image out: strings/bytes/children point into <paramref name="arena"/>. Empty input is the default message: the image stays zeroed and no native call runs.</summary>
    public void Decode(byte[] wire, byte* image, IntPtr arena, int arenaBytes)
    {
        if (wire.Length == 0)
        {
            return;
        }
        int status;
        fixed (byte* data = wire)
        {
            status = Native.pbw_cs_shim_decode(_handle, (IntPtr)data, (nuint)wire.Length, (IntPtr)image,
                arena, (nuint)arenaBytes);
        }
        if (status == 2)
        {
            throw new PbwireException(status, "decode arena too small (raise ImageCall.Rent's decode bytes)");
        }
        Native.Check(status);
    }
}

/// <summary>
/// The unmanaged memory of one flat-image call: the image itself (zeroed on
/// every rent), a bump arena for the strings/bytes/sub-images an encode image
/// points at, and the block the C decoder bump-allocates from. Reused per
/// thread, so a steady-state loop allocates nothing; dispose in a
/// <c>finally</c>/<c>using</c> before reading a decoded image.
/// </summary>
public sealed unsafe class ImageCall : IDisposable
{
    [ThreadStatic] private static ImageCall? _cached;
    private byte* _image;
    private int _imageBytes;
    private byte* _arena;
    private int _arenaBytes;
    private ImageArena? _bump;

    private ImageCall()
    {
    }

    /// <summary>Rents this thread's call block: the image plus a decode arena of at least <paramref name="decodeBytes"/>.</summary>
    public static ImageCall Rent(int imageBytes, int decodeBytes = 0)
    {
        var call = _cached ?? new ImageCall();
        _cached = null;
        call.Grow(imageBytes, decodeBytes);
        new Span<byte>(call._image, imageBytes).Clear();
        call._bump!.Reset();
        return call;
    }

    /// <summary>The flat image (at least the requested bytes, zeroed).</summary>
    public byte* Image => _image;

    /// <summary>Where a generated <c>WriteImage</c> copies strings, bytes and sub-images.</summary>
    public ImageArena Arena => _bump!;

    /// <summary>The block the C decoder bump-allocates from.</summary>
    public IntPtr DecodeArena => (IntPtr)_arena;

    public int DecodeArenaBytes => _arenaBytes;

    private void Grow(int imageBytes, int decodeBytes)
    {
        if (_image == null || _imageBytes < imageBytes)
        {
            if (_image != null)
            {
                Marshal.FreeHGlobal((IntPtr)_image);
            }
            _imageBytes = Math.Max(imageBytes, 4096);
            _image = (byte*)Marshal.AllocHGlobal(_imageBytes);
        }
        if (_arena == null || _arenaBytes < decodeBytes)
        {
            if (_arena != null)
            {
                Marshal.FreeHGlobal((IntPtr)_arena);
            }
            _arenaBytes = Math.Max(decodeBytes, 4096);
            _arena = (byte*)Marshal.AllocHGlobal(_arenaBytes);
        }
        _bump ??= new ImageArena();
    }

    public void Dispose() => _cached = this;
}

/// <summary>
/// A bump allocator over a chain of unmanaged blocks, rewound per call: the
/// values an encode image borrows. A block that fills up is kept (never freed
/// while a pointer may still point into it) and the next one is started, so a
/// steady-state loop allocates nothing after the first pass at that size.
/// </summary>
public sealed unsafe class ImageArena
{
    private IntPtr[] _blocks = new IntPtr[4];
    private int[] _caps = new int[4];
    private int _count;
    private int _index;
    private int _used;

    /// <summary>Rewinds to the first block; the blocks stay for the next call.</summary>
    public void Reset()
    {
        _index = 0;
        _used = 0;
    }

    /// <summary>8-byte aligned, at least <paramref name="size"/> bytes; storage is not zeroed.</summary>
    public byte* Alloc(int size)
    {
        var need = (size + 7) & ~7;
        while (true)
        {
            if (_index < _count)
            {
                if (_used + need <= _caps[_index])
                {
                    var at = (byte*)_blocks[_index] + _used;
                    _used += need;
                    return at;
                }
                _index++;
                _used = 0;
                continue;
            }
            var fresh = Math.Max(need, 65536);
            if (_count == _blocks.Length)
            {
                Array.Resize(ref _blocks, _count * 2);
                Array.Resize(ref _caps, _count * 2);
            }
            _blocks[_count] = Marshal.AllocHGlobal(fresh);
            _caps[_count] = fresh;
            _count++;
        }
    }

    /// <summary>Copies bytes (NUL-guarded); zero bytes still get a non-NULL pointer.</summary>
    public IntPtr Copy(ReadOnlySpan<byte> bytes)
    {
        var at = Alloc(bytes.Length + 1);
        bytes.CopyTo(new Span<byte>(at, bytes.Length));
        at[bytes.Length] = 0;
        return (IntPtr)at;
    }

    /// <summary>A NUL-terminated UTF-8 copy that is never NULL: empty text takes one NUL byte, so a set-but-empty value stays visible to the native reader.</summary>
    public IntPtr Utf8OrEmpty(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            var at = Alloc(1);
            at[0] = 0;
            return (IntPtr)at;
        }
        return Utf8(text);
    }

    /// <summary>A NUL-terminated UTF-8 copy, or NULL for an absent value.</summary>
    public IntPtr Utf8(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return IntPtr.Zero;
        }
        var n = System.Text.Encoding.UTF8.GetByteCount(text);
        var at = Alloc(n + 1);
        System.Text.Encoding.UTF8.GetBytes(text, new Span<byte>(at, n));
        at[n] = 0;
        return (IntPtr)at;
    }

    /// <summary>Zeroes a range (a fresh sub-image before its owner writes it).</summary>
    public static void Clear(byte* at, int size) => new Span<byte>(at, size).Clear();
}
