# Four standard tiers vs elixir-protobuf (`Protobuf`), on the same
# `matrix3.proto` message and payload every other binding measures
# (BENCHMARKS.md at the repo root).
#
# Run from bindings/elixir (needs Elixir + OTP, `make setup` once):
#
#     mix run benchmarks/bench.exs
#
# Every backend must produce the pinned 136 / 174 / 2469 / 74913 bytes
# (encode, plus re-encoding a decoded reference) before anything is timed.
# Each op is calibrated from its tier floor until one pass takes at least
# 0.25 s, then the best of 3 timed samples is reported; throughput is MB/s
# over serialized wire size. Same unified standard as every other binding.

defmodule Bench.Color3 do
  use Protobuf, enum: true, syntax: :proto3
  field(:COLOR3_UNSPECIFIED, 0)
  field(:COLOR3_RED, 1)
  field(:COLOR3_GREEN, 2)
end

defmodule Bench.Inner3 do
  use Protobuf, syntax: :proto3
  field(:name, 1, type: :string)
  field(:value, 2, type: :int32)
end

defmodule Bench.Scalars3 do
  use Protobuf, syntax: :proto3
  field(:i32, 1, type: :int32)
  field(:i64, 2, type: :int64)
  field(:u32, 3, type: :uint32)
  field(:u64, 4, type: :uint64)
  field(:s32, 5, type: :sint32)
  field(:s64, 6, type: :sint64)
  field(:fx32, 7, type: :fixed32)
  field(:fx64, 8, type: :fixed64)
  field(:sfx32, 9, type: :sfixed32)
  field(:sfx64, 10, type: :sfixed64)
  field(:f32, 11, type: :float)
  field(:f64, 12, type: :double)
  field(:flag, 13, type: :bool)
  field(:str, 14, type: :string)
  field(:color, 15, type: Bench.Color3, enum: true)
  field(:inner, 16, type: Bench.Inner3)
  field(:nums, 17, repeated: true, type: :int32)
  field(:tags, 18, repeated: true, type: :string)
  field(:inners, 19, repeated: true, type: Bench.Inner3)
  field(:opt_i32, 20, proto3_optional: true, type: :int32)
  field(:opt_str, 21, proto3_optional: true, type: :string)
  field(:flags, 22, repeated: true, type: :bool)
end

defmodule Bench do
  alias Bench.{Inner3, Scalars3}

  @tiers [
    {"small", 0, 136, 50_000},
    {"medium", 1, 174, 50_000},
    {"large", 100, 2469, 5_000},
    {"xlarge", 3000, 74_913, 500}
  ]
  @floor_seconds 0.25
  @samples 3

  def run do
    :ok = Pbwire.load_schema([Scalars3])

    for {label, n, want, floor} <- @tiers do
      msg = scalars3(n)
      ref = Protobuf.encode(msg)

      unless byte_size(ref) == want do
        fail("#{label}: oracle is #{byte_size(ref)} bytes, want #{want}")
      end

      pb = Pbwire.encode(msg)

      unless pb == ref do
        fail("#{label}: Pbwire bytes differ from elixir-protobuf")
      end

      unless Pbwire.encode(Pbwire.decode(pb, Scalars3)) == ref do
        fail("#{label}: decode/re-encode round trip differs")
      end

      ref_iters = calibrate(floor, fn -> Pbwire.encode(msg) end)

      IO.puts("#{label}: #{want} bytes, #{ref_iters} iterations (repeat field x#{n})")
      IO.puts("  impl       op           ns/op       MB/s")
      time("pbwire", "encode", floor, fn -> Pbwire.encode(msg) end, want)
      time("protobuf", "encode", floor, fn -> Protobuf.encode(msg) end, want)
      time("pbwire", "decode", floor, fn -> Pbwire.decode(ref, Scalars3) end, want)
      time("protobuf", "decode", floor, fn -> Protobuf.decode(ref, Scalars3) end, want)
    end
  end

  # The matrix3.txt payload: every singular scalar non-default on purpose
  # (proto3 implicit presence has no has-flag), repeated fields scaled by n.
  defp scalars3(n) do
    %Scalars3{
      i32: -2_147_483_648,
      i64: 9_223_372_036_854_775_807,
      u32: 4_294_967_295,
      u64: 18_446_744_073_709_551_615,
      s32: -2_147_483_648,
      s64: -9_223_372_036_854_775_808,
      fx32: 3_735_928_559,
      fx64: 72_623_859_790_382_856,
      sfx32: -2_147_483_648,
      sfx64: -9_223_372_036_854_775_808,
      f32: :infinity,
      f64: -123_456.789,
      flag: true,
      str: "hello",
      color: :COLOR3_GREEN,
      inner: %Inner3{name: "kid", value: 42},
      nums: for(i <- 0..(n - 1)//1, do: i - 3),
      tags: List.duplicate("tag", n),
      inners: for(i <- 0..(n - 1)//1, do: %Inner3{name: "grandkid", value: i + 1}),
      opt_i32: -7,
      opt_str: "opt",
      flags: for(i <- 0..(n - 1)//1, do: rem(i, 2) == 0)
    }
  end

  defp time(impl, op, floor, fun, bytes) do
    fun.()
    iters = calibrate(floor, fun)
    best = Enum.min(for _ <- 1..@samples, do: measure(iters, fun))
    ns_per_op = best / iters
    mbs = bytes / ns_per_op * 1000

    :io.format("  ~-10s ~-8s ~12.1f ~10.1f~n", [
      impl,
      op,
      ns_per_op,
      mbs
    ])
  end

  defp calibrate(floor, fun) do
    Stream.iterate(floor, &(&1 * 2))
    |> Enum.find(fn iters -> measure(iters, fun) / 1_000_000_000 >= @floor_seconds end)
  end

  defp measure(iters, fun) do
    {us, :ok} =
      :timer.tc(fn ->
        for _ <- 1..iters, do: fun.()
        :ok
      end)

    us * 1000
  end

  defp fail(reason) do
    IO.puts(:stderr, "FAIL: #{reason}")
    System.halt(1)
  end
end

Bench.run()
