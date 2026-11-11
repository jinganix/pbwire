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

    static Native() =>
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, _, _) =>
            name == Lib ? NativeLibrary.Load(LibraryPath()) : IntPtr.Zero);

    /// <summary>PBWIRE_LIB, else build/libpbwire.{dylib,so} found above the working directory.</summary>
    internal static string LibraryPath()
    {
        var env = Environment.GetEnvironmentVariable("PBWIRE_LIB");
        if (!string.IsNullOrEmpty(env))
        {
            return env;
        }
        for (var dir = new DirectoryInfo(Directory.GetCurrentDirectory()); dir != null; dir = dir.Parent)
        {
            foreach (var suffix in new[] { "dylib", "so" })
            {
                var candidate = Path.Combine(dir.FullName, "build", $"libpbwire.{suffix}");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }
        throw new PbwireException(-1, "no shared library found (set PBWIRE_LIB; run `make shared`)");
    }

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

    /// <summary>Resolves a field once and caches it: a later set/get costs one native call.</summary>
    public Field Field(string name)
    {
        EnsureOpen();
        if (_fields.TryGetValue(name, out var cached))
        {
            return cached;
        }
        var ptr = Native.pbw_descriptor_find_field_name(_desc, name);
        if (ptr == IntPtr.Zero)
        {
            throw new PbwireException(-1, $"{Name}: no such field: {name}");
        }
        return _fields[name] = new Field(this, ptr);
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
        try
        {
            Native.PbwVisitFn fn = (IntPtr ud, IntPtr field, uint number, uint wireType, ref PbwVisitValue value) =>
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
        return ptr == IntPtr.Zero ? null : new ReadOnlySpan<byte>((void*)ptr, (int)len).ToArray();
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
        return value == IntPtr.Zero ? 0 : (int)Native.pbw_value_count(value);
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
