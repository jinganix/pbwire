// protobuf-net baseline (the fastest widely used C# runtime): hand-written
// contracts matching matrix3.proto field for field, so the bytes must equal
// the Google.Protobuf reference before anything is timed.
using ProtoBuf;

namespace Pbn;

[ProtoContract]
public class Inner3
{
    [ProtoMember(1)]
    public string Name { get; set; } = "";

    [ProtoMember(2)]
    public int Value { get; set; }
}

[ProtoContract]
public class Scalars3
{
    [ProtoMember(1)]
    public int I32 { get; set; }

    [ProtoMember(2)]
    public long I64 { get; set; }

    [ProtoMember(3)]
    public uint U32 { get; set; }

    [ProtoMember(4)]
    public ulong U64 { get; set; }

    [ProtoMember(5, DataFormat = DataFormat.ZigZag)]
    public int S32 { get; set; }

    [ProtoMember(6, DataFormat = DataFormat.ZigZag)]
    public long S64 { get; set; }

    [ProtoMember(7, DataFormat = DataFormat.FixedSize)]
    public uint Fx32 { get; set; }

    [ProtoMember(8, DataFormat = DataFormat.FixedSize)]
    public ulong Fx64 { get; set; }

    [ProtoMember(9, DataFormat = DataFormat.FixedSize)]
    public int Sfx32 { get; set; }

    [ProtoMember(10, DataFormat = DataFormat.FixedSize)]
    public long Sfx64 { get; set; }

    [ProtoMember(11)]
    public float F32 { get; set; }

    [ProtoMember(12)]
    public double F64 { get; set; }

    [ProtoMember(13)]
    public bool Flag { get; set; }

    [ProtoMember(14)]
    public string Str { get; set; } = "";

    [ProtoMember(15)]
    public int Color { get; set; }

    [ProtoMember(16)]
    public Inner3? Inner { get; set; }

    [ProtoMember(17, IsPacked = true)]
    public List<int>? Nums { get; set; }

    [ProtoMember(18)]
    public List<string> Tags { get; } = new();

    [ProtoMember(19)]
    public List<Inner3> Inners { get; } = new();

    [ProtoMember(20)]
    public int? OptI32 { get; set; }

    [ProtoMember(21)]
    public string? OptStr { get; set; }

    [ProtoMember(22, IsPacked = true)]
    public List<bool>? Flags { get; set; }
}
