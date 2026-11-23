// Self-check against the pinned protoc bytes: build structtest.Scalars3 from
// bindings/c/tests/data/matrix3.txt, encode, compare with matrix3.bin (the same
// oracle the C binding uses), then decode it back and check the values.
//
//   dotnet run --project tests -- <descriptor-set>

using Pbwire;
using System.Diagnostics;

var desc = args.Length > 0 ? args[0] : "../../c/tests/data/matrix3.desc";
var oracle = File.ReadAllBytes(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(desc))!, "matrix3.bin"));
var checks = 0;

void Check(bool ok, string what)
{
    checks++;
    if (!ok)
    {
        Console.Error.WriteLine($"FAIL {what}");
        Environment.Exit(1);
    }
}

void FillInto(Message msg, MessageType innerTy)
{
    var name = innerTy.Field("name");
    var value = innerTy.Field("value");

    msg.Set("i32", int.MinValue)
        .Set("i64", long.MaxValue)
        .Set("u32", uint.MaxValue)
        .Set("u64", -1L) // u64::MAX bits
        .Set("s32", int.MinValue)
        .Set("s64", long.MinValue)
        .Set("fx32", 0xdeadbeefL)
        .Set("fx64", 72623859790382856L)
        .Set("sfx32", int.MinValue)
        .Set("sfx64", long.MinValue)
        .Set("f32", float.PositiveInfinity)
        .Set("f64", -123456.789)
        .Set("flag", true)
        .Set("str", "hello")
        .Set("color", 2L)
        .Set("opt_i32", -7L)
        .Set("opt_str", "opt");

    foreach (var n in new long[] { 1, -5, 300 })
    {
        msg.Set("nums", n);
    }
    foreach (var tag in new[] { "a", "bb" })
    {
        msg.Set("tags", tag);
    }
    foreach (var (innerName, innerValue) in new[] { ("x", 1L), ("y", -2L) })
    {
        msg.SetMessage("inners").Set(name, innerName).Set(value, innerValue);
    }
    foreach (var flag in new[] { true, false, true })
    {
        msg.Set("flags", flag);
    }
    msg.SetMessage("inner").Set(name, "kid").Set(value, 42L);
}

byte[] Fill(MessageType ty, MessageType innerTy)
{
    using var msg = ty.NewMessage();
    FillInto(msg, innerTy);
    return msg.Encode();
}

// Concurrent stress over the matrix3 oracle: 8 workers x 200 iters, three
// phases per worker mirroring the C/C++/Rust/Zig/Swift shape: (a)
// independent-message encode == want bytes over the shim, (b) SHARED
// read-only message concurrent encode == want bytes, (c) fresh-target decode
// of want with field checks (NOT re-encode).
//
// Only the schema types and the want bytes cross threads. MessageType.Field
// takes a lock around its cache fill (like LayoutSource.Plan), so worker name
// lookups are safe even cold: the serial baseline below is the oracle pin
// only, not a cache warmer. Concurrent encodes of one native tree
// are safe: the C core keeps no mutable encode state (proven by
// bindings/c/tests/test_concurrency.c). Timing prints only, never asserted.
void Concurrency(Schema schema, MessageType ty, MessageType innerTy, byte[] want)
{
    const int Workers = 8;
    const int Iters = 200;
    // Every field the matrix3 fixture touches: resolving the full set per
    // iteration hammers the cache fill from all workers at once.
    string[] names = new[]
    {
        "i32", "i64", "u32", "u64", "s32", "s64", "fx32", "fx64",
        "sfx32", "sfx64", "f32", "f64", "flag", "str", "color",
        "opt_i32", "opt_str", "nums", "tags", "inners", "inner", "flags",
    };

    // COLD-cache phase first: one FRESH type pair with no prior Field lookups
    // on any thread, shared across workers that concurrently resolve the full
    // field-name set and encode 200 iters each. This is the real server pattern
    // (a singleton type shared across first-burst request threads); concurrent
    // cold fills used to race on the Dictionary, so this phase exercises the
    // lock. Resolution must match the warm pair field-for-field and the bytes
    // must equal want.
    // COLD-cache phase first: one FRESH type pair with no prior Field lookups
    // on any thread, shared across workers that concurrently resolve the full
    // field-name set and encode 200 iters each. This is the real server pattern
    // (a singleton type shared across first-burst request threads); concurrent
    // cold fills used to race on the Dictionary, so this phase exercises the
    // lock. Resolution must match the warm pair field-for-field and the bytes
    // must equal want.
    var coldTy = schema.Message("structtest.Scalars3");
    var coldInner = schema.Message("structtest.Inner3");
    var coldProblems = new List<string>();
    // Worker count tracks ACTUAL parallelism (Julia precedent
    // `min(8, nthreads())`): `Parallel.For(0, 8, ...)` does not guarantee 8
    // concurrent workers, so a `Barrier(8)` start gate can deadlock on hosts
    // with < 8 cores (first wave blocks in the barrier while the remaining
    // iterations queue on the same blocked workers). The spin/wait start gate
    // below waits on this same count, so gate and parallelism cannot disagree.
    var workers = Math.Min(Workers, Environment.ProcessorCount);
    var arrived = 0;
    var coldSw = Stopwatch.StartNew();
    Parallel.For(0, workers, worker =>
    {
        Interlocked.Increment(ref arrived);
        var spin = new SpinWait();
        while (Volatile.Read(ref arrived) < workers) spin.SpinOnce();
        for (var i = 0; i < Iters; i++)
        {
            try
            {
                foreach (var n in names)
                {
                    if (coldTy.Field(n).Number != ty.Field(n).Number)
                    {
                        lock (coldProblems) { coldProblems.Add($"worker {worker}: cold field {n} resolved differently"); }
                        break;
                    }
                }
                if (coldInner.Field("name").Number != innerTy.Field("name").Number
                    || coldInner.Field("value").Number != innerTy.Field("value").Number)
                {
                    lock (coldProblems) { coldProblems.Add($"worker {worker}: cold inner field resolved differently"); }
                }
                if (!Fill(coldTy, coldInner).SequenceEqual(want))
                {
                    lock (coldProblems) { coldProblems.Add($"worker {worker}: cold independent-encode != want bytes"); }
                }
                Interlocked.Increment(ref checks);
            }
            catch (Exception e)
            {
                lock (coldProblems) { coldProblems.Add($"worker {worker}: cold threw {e.GetType().Name}"); }
            }
        }
    });
    coldSw.Stop();
    var coldTotal = workers * Iters;
    Console.WriteLine($"OK   concurrent cold-cache: {coldTotal} resolves+encodes in {coldSw.Elapsed.TotalSeconds:F3} s ({coldTotal / coldSw.Elapsed.TotalSeconds:F0}/s)");
    Check(coldProblems.Count == 0, "concurrency cold-cache: " + string.Join("; ", coldProblems.Take(8)));

    // Serial encode-once baseline: must equal the protoc oracle before any
    // worker starts, so a mapping bug fails here and not in a thread.
    Check(Fill(ty, innerTy).SequenceEqual(want), "concurrency serial encode equals the protoc oracle");

    // Shared read-only fixture for phase (b): filled once here, never mutated
    // while workers run.
    using var shared = ty.NewMessage();
    FillInto(shared, innerTy);

    var problems = new List<string>();
    var sw = Stopwatch.StartNew();
    Parallel.For(0, workers, worker =>
    {
        for (var i = 0; i < Iters; i++)
        {
            // (a) independent-message encode == want bytes: each iteration
            // owns its message; nothing crosses threads but ty/innerTy/want.
            try
            {
                if (!Fill(ty, innerTy).SequenceEqual(want))
                {
                    lock (problems) { problems.Add($"worker {worker}: phase (a) independent-encode != want bytes"); }
                }
                Interlocked.Increment(ref checks);
            }
            catch (Exception e)
            {
                lock (problems) { problems.Add($"worker {worker}: phase (a) threw {e.GetType().Name}"); }
            }
            // (b) SHARED read-only message concurrent encode == want bytes.
            try
            {
                if (!shared.Encode().SequenceEqual(want))
                {
                    lock (problems) { problems.Add($"worker {worker}: phase (b) shared-read-only-encode != want bytes"); }
                }
                Interlocked.Increment(ref checks);
            }
            catch (Exception e)
            {
                lock (problems) { problems.Add($"worker {worker}: phase (b) threw {e.GetType().Name}"); }
            }
            // (c) fresh-target decode with field checks, NOT re-encode.
            try
            {
                using var back = ty.Decode(want);
                Interlocked.Increment(ref checks);
                var ok = back.GetInt("i32") == int.MinValue
                    && back.GetBool("flag")
                    && back.GetString("str") == "hello"
                    && back.GetDouble("f64") == -123456.789
                    && back.GetLong("color") == 2
                    && back.GetLong("opt_i32") == -7
                    && back.Count("nums") == 3
                    && back.At("nums", 1)?.AsLong() == -5
                    && back.At("tags", 1)?.AsString() == "bb"
                    && back.At("inners", 1)?.AsMessage()?.GetLong("value") == -2
                    && back.At("flags", 1)?.AsBool() == false
                    && back.GetMessage("inner")?.GetString("name") == "kid";
                if (!ok)
                {
                    lock (problems) { problems.Add($"worker {worker}: phase (c) fresh-target decode field mismatch"); }
                }
            }
            catch (Exception e)
            {
                lock (problems) { problems.Add($"worker {worker}: phase (c) threw {e.GetType().Name}"); }
            }
        }
    });
    sw.Stop();
    var total = workers * Iters * 3;
    Console.WriteLine($"OK   concurrent: {total} encodes+decodes in {sw.Elapsed.TotalSeconds:F3} s ({total / sw.Elapsed.TotalSeconds:F0}/s)");
    Check(problems.Count == 0, "concurrency: " + string.Join("; ", problems.Take(8)));
}

using var schema = Schema.FromFile(desc);
var ty = schema.Message("structtest.Scalars3");
var innerTy = schema.Message("structtest.Inner3");

var wire = Fill(ty, innerTy);
Check(wire.SequenceEqual(oracle), $"encoded bytes differ from protoc ({wire.Length} vs {oracle.Length})");
Console.WriteLine($"OK   encode matches protoc, {oracle.Length} bytes");

using (var decoded = ty.Decode(oracle))
{
    Check(decoded.GetInt("i32") == int.MinValue, "i32");
    Check(decoded.GetBool("flag"), "flag");
    Check(decoded.GetString("str") == "hello", "str");
    Check(decoded.GetDouble("f64") == -123456.789, "f64");
    Check(decoded.GetLong("color") == 2, "color");
    Check(decoded.GetLong("opt_i32") == -7, "opt_i32");
    Check(decoded.Has("opt_str") && !ty.NewMessage().Has("i32"), "presence");
    Check(decoded.Count("nums") == 3, "nums count");
    Check(decoded.At("nums", 1)?.AsLong() == -5, "nums[1]");
    Check(decoded.At("tags", 1)?.AsString() == "bb", "tags[1]");
    Check(decoded.At("inners", 1)?.AsMessage()?.GetLong("value") == -2, "inners[1].value");
    Check(decoded.At("flags", 1)?.AsBool() == false, "flags[1]");
    Check(decoded.GetMessage("inner")?.GetString("name") == "kid", "inner.name");
    Check(decoded.Encode().SequenceEqual(oracle), "decode -> encode is byte-exact");
    Console.WriteLine("OK   decode round trip, repeated and nested fields");
}

// A field the schema does not declare (99, varint 7) survives re-encoding.
var unknown = new byte[] { 0x98, 0x06, 0x07 };
using (var innerOnly = innerTy.Decode(unknown))
{
    Check(innerOnly.Encode().SequenceEqual(unknown), "unknown field preserved");
}

// Bad input is an error, not a crash.
Check(Throws(() => ty.Decode(new byte[] { 0x80 })), "truncated tag accepted");
Check(Throws(() => schema.Message("structtest.Nope")), "unknown message accepted");
Check(Throws(() => ty.Field("nope")), "unknown field accepted");

// The extended surface: deterministic/delimited encode, JSON and text
// print/parse, bounded decode, the single-level visit scan, and service
// routing metadata.
using (var m = ty.NewMessage())
{
    m.Set("str", "hello");
    var w = m.Encode();
    Check(m.EncodedSize() == (ulong)w.Length, "encoded size");
    Check(m.IsInitialized(), "initialized");
    Check(m.EncodeDeterministic().SequenceEqual(w), "deterministic");
    var framed = m.EncodeDelimited();
    var both = framed.Concat(framed).ToArray();
    var (back, consumed) = ty.DecodeDelimited(both);
    using (back)
    {
        Check(back.Encode().SequenceEqual(w), "delimited round trip");
    }
    Check(consumed == framed.Length, "delimited consumed");
    var js = m.PrintJson();
    using (var fromJson = ty.ParseJson(js))
    {
        Check(fromJson.Encode().SequenceEqual(w), "json round trip");
    }
    Check(Throws(() => ty.ParseJson("{\"nope\":1}")), "json unknown accepted");
    Check(Throws(() => ty.ParseJson(js, false, 1)), "ParseJson honors maxBytes");
    using (var bounded = ty.ParseJson(js, false, (ulong)js.Length + 1))
    {
        Check(bounded.EncodedSize() > 0, "ParseJson accepts an adequate maxBytes");
    }
    using (var ignored = ty.ParseJson("{\"nope\":1}", ignoreUnknown: true))
    {
        Check(ignored.Encode().Length == 0, "json ignore unknown");
    }
    var tx = m.PrintText();
    // PrintText(false) must drop unknown fields; the default used to print them.
    var withUnknown = new byte[w.Length + 3];
    Array.Copy(w, withUnknown, w.Length);
    withUnknown[w.Length] = 0xF8; // field 111, varint 2
    withUnknown[w.Length + 1] = 0x06;
    withUnknown[w.Length + 2] = 0x02;
    using (var decoded = ty.Decode(withUnknown))
    {
        Check(decoded.PrintText(true).Contains("111: 2"),
            "PrintText(true) prints unknowns");
        Check(!decoded.PrintText(false).Contains("111"),
            "PrintText(false) drops unknowns");
    }
    Check(Throws(() => ty.ParseText(tx, 1)), "ParseText honors maxBytes");
    using (var fromText = ty.ParseText(tx))
    {
        Check(fromText.Encode().SequenceEqual(w), "text round trip");
    }
    using (var limited = ty.DecodeLimits(w))
    {
        Check(limited.Encode().SequenceEqual(w), "limits defaults");
    }
    Check(Throws(() => ty.DecodeLimits(w, maxBytes: 1)), "limits maxBytes ignored");
    var seen = 0;
    ty.Visit(w, f => { seen++; return f.Number != 0; });
    Check(seen > 0, "visit saw fields");
    // A callback exception must not unwind through the native frame; the
    // scan stops and the exception surfaces to the caller unchanged.
    {
        var thrown = new InvalidOperationException("boom");
        var caught = false;
        try
        {
            ty.Visit(w, f => throw thrown);
        }
        catch (InvalidOperationException e)
        {
            caught = ReferenceEquals(e, thrown);
        }
        Check(caught, "visit rethrows the callback exception");
        var again = 0;
        ty.Visit(w, f => { again++; return true; });
        Check(again > 0, "visit still usable after a throw");
    }
    Check(schema.ServiceCount() == 0, "no services in fixture");
    Check(Throws(() => schema.ResolveMethod("no.Such", "Nope")), "unknown service accepted");
}

// The cases a binding can get wrong even when the core is right: payloads with
// embedded NULs / invalid UTF-8, non-ASCII text, a negative enum, an empty
// repeated element, proto3 optional presence with an empty value, and oneof
// switching. tests/data/edge.bin is protoc --encode.
// Walk up to the checkout: `dotnet run` keeps the caller's working directory.
static string FindRepo()
{
    for (var dir = new DirectoryInfo(Directory.GetCurrentDirectory()); dir != null; dir = dir.Parent)
    {
        if (File.Exists(Path.Combine(dir.FullName, "tests/data/edge.desc")))
        {
            return dir.FullName;
        }
    }
    throw new InvalidOperationException("no pbwire checkout above the working directory");
}

var root = FindRepo();
var edgeOracle = File.ReadAllBytes(Path.Combine(root, "tests/data/edge.bin"));
using var edgeSchema = Schema.FromFile(Path.Combine(root, "tests/data/edge.desc"));
var edgeTy = edgeSchema.Message("edge.Edge");
var edgeInner = edgeSchema.Message("edge.Inner");

using (var edge = edgeTy.NewMessage())
{
    edge.Set("raw", new byte[] { 0x00, 0x01, 0xff })
        .Set("text", "héllo ☃")
        .Set("sign", -3L)
        .Set("maybe", "")
        .Set("pick_num", 5L) // oneof: replaced below
        .Set("pick_str", "one");
    foreach (var tag in new[] { "a", "", "b" })
    {
        edge.Set("tags", tag);
    }
    edge.Set("blobs", new byte[] { 0x00 });
    edge.Set("blobs", Array.Empty<byte>());
    edge.SetMessage("inner").Set("tag", "kid").Set("n", -1L);

    Check(edge.Encode().SequenceEqual(edgeOracle), "edge payload differs from protoc");
}

using (var edge = edgeTy.Decode(edgeOracle))
{
    Check(edge.GetBytes("raw")!.SequenceEqual(new byte[] { 0x00, 0x01, 0xff }), "bytes with NUL and 0xff");
    Check(edge.GetString("text") == "héllo ☃", "non-ASCII string");
    Check(edge.GetLong("sign") == -3, "negative enum");
    Check(edge.Count("tags") == 3, "empty repeated element count");
    Check(edge.At("tags", 1)?.AsString() == "", "empty repeated element");
    Check(edge.At("blobs", 0)?.AsBytes()!.SequenceEqual(new byte[] { 0x00 }) == true, "bytes element with NUL");
    Check(edge.At("blobs", 1)?.AsBytes()!.Length == 0, "empty bytes element");
    Check(edge.GetMessage("inner")?.GetLong("n") == -1, "nested negative");
    Check(edge.Has("maybe") && edge.GetString("maybe") == "", "optional present and empty");
    Check(edge.Has("pick_str") && !edge.Has("pick_num"), "oneof keeps only the last member");
    Check(edge.Encode().SequenceEqual(edgeOracle), "edge decode -> encode is byte-exact");
}
Console.WriteLine("OK   edge payload: bytes, UTF-8, negative enum, empty elements, oneof");

// A default message is zero bytes both ways.
Check(edgeTy.NewMessage().Encode().Length == 0, "empty message encodes to zero bytes");
using (var emptyDecoded = edgeTy.Decode(Array.Empty<byte>()))
{
    Check(emptyDecoded.Encode().Length == 0, "empty wire decodes and re-encodes to zero bytes");
    Check(!emptyDecoded.Has("text"), "empty message has no fields");
}

// A wrong value kind is refused, not written.
Check(Rejects(() => edgeTy.NewMessage().Set("pick_num", "not a number"), "type mismatch"),
    "wrong-typed value rejected");
Check(Rejects(() => edgeTy.NewMessage().SetMessage("text"), "not a message field"),
    "setMessage on a scalar rejected");

// Lifetime: a disposed Schema frees the descriptors every Type/Field points
// into, and a disposed root frees the arena a nested view points into. Both
// must throw instead of reading freed memory.
{
    var doomed = Schema.FromFile(desc);
    var doomedTy = doomed.Message("structtest.Inner3");
    var doomedField = doomedTy.Field("value");
    var doomedMsg = doomedTy.NewMessage();
    doomed.Dispose();
    Check(Throws(() => doomedTy.NewMessage()), "disposed schema rejects NewMessage");
    Check(Throws(() => { _ = doomedField.Number; }), "disposed schema rejects Field.Number");
    Check(Throws(() => doomedMsg.Encode()), "disposed schema rejects encode");
}
{
    var live = Schema.FromFile(desc);
    var liveTy = live.Message("structtest.Scalars3");
    var liveRoot = liveTy.NewMessage();
    var view = liveRoot.SetMessage("inner");
    liveRoot.Dispose();
    Check(Throws(() => view.Set("name", "x")), "view after root dispose rejects writes");
    view.Dispose();
    live.Dispose();
}

Concurrency(schema, ty, innerTy, oracle);

var coreChecks = Pbwire.Test.CoreTests.Run(Path.GetDirectoryName(Path.GetFullPath(desc))!);
Console.WriteLine($"OK   pure core: {coreChecks} checks passed");
Console.WriteLine($"OK   {checks} checks passed");

static bool Rejects(Action op, string expected)
{
    try
    {
        op();
        return false;
    }
    catch (PbwireException e)
    {
        return e.Message.Contains(expected);
    }
}

static bool Throws(Action op)
{
    try
    {
        op();
        return false;
    }
    catch (PbwireException)
    {
        return true;
    }
    catch (ObjectDisposedException)
    {
        return true;
    }
}
