// Public surface of the pure-C# core: a Schema loaded from a
// FileDescriptorSet, message types that decode and encode wire bytes, and the
// plain-object helpers the pbench core arm uses. No native library, no protoc
// plugin, no generated code, no NuGet dependency.
//
//   using var schema = Pbwire.Core.Schema.FromFile("matrix3.desc");
//   var type = schema.Message("structtest.Scalars3");
//   var msg = type.Decode(bytes);   // unknown fields retained
//   type.Encode(msg);               // byte-exact for a decoded tree

namespace Pbwire.Core;

/// <summary>One message type: NewMessage() plus binary decode/encode.</summary>
public sealed class MessageType
{
    public MessageType(MsgDef def) => Def = def;

    public MsgDef Def { get; }

    /// <summary>Fully qualified name, e.g. "structtest.Scalars3".</summary>
    public string Name() => Def.FullName;

    /// <summary>An empty, mutable message of this type.</summary>
    public Msg NewMessage() => Decoder.NewMessage(Def);

    /// <summary>
    /// Decodes wire bytes; unknown fields are kept and re-encoded. The bytes are
    /// the authority on presence, so a field absent on the wire stays absent.
    /// </summary>
    public Msg Decode(byte[] wire, Limits? limits = null) => Decoder.Decode(Def, wire, limits);

    /// <summary>Decodes one body slice (offset + length) of a larger buffer.</summary>
    public Msg DecodeSlice(byte[] wire, int off, int len, Limits? limits = null) =>
        Decoder.DecodeSlice(Def, wire, off, len, limits);

    /// <summary>Encodes a tree; a tree that came from Decode() re-emits byte-exactly.</summary>
    public byte[] Encode(Msg m) => Encoder.Encode(m);

    /// <summary>Decodes straight into a plain object (proto field names, long/ulong).</summary>
    public Dictionary<string, object?> DecodeObject(byte[] wire, Limits? limits = null) =>
        Tree.ToObject(Decode(wire, limits));

    /// <summary>Encodes a plain object (the inverse of DecodeObject).</summary>
    public byte[] EncodeObject(IDictionary<string, object?> obj) =>
        Encoder.Encode(Tree.FromObject(Def, obj));

    /// <summary>The field named <paramref name="name"/>, or null.</summary>
    public FieldDef? Field(string name) => Descriptor.FieldByName(Def, name);

    public override string ToString() => Name();
}

/// <summary>A loaded FileDescriptorSet: the runtime schema every type comes from.</summary>
public sealed class Schema
{
    private readonly CoreSchema _core;

    private Schema(CoreSchema core) => _core = core;

    public CoreSchema Core => _core;

    /// <summary>Loads a descriptor set written by protoc --descriptor_set_out.</summary>
    public static Schema FromFile(string path, Limits? limits = null) =>
        FromBytes(File.ReadAllBytes(path), limits);

    /// <summary>Loads descriptor-set bytes; use --include_imports so every type resolves.</summary>
    public static Schema FromBytes(byte[] descriptorSet, Limits? limits = null) =>
        new(Descriptor.LoadSchema(descriptorSet, limits));

    /// <summary>Looks up a message by its fully qualified name.</summary>
    public MessageType Message(string fullName) => new(_core.Message(fullName));
}
