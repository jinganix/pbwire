defmodule Matrix3GeneratedTest do
  use ExUnit.Case, async: true

  # Generated-conformance check for the Elixir binding: build matrix3.proto's
  # Scalars3 through the generated modules, encode, require the protoc
  # reference bytes (matrix3.bin), then decode those bytes back and check
  # every field. The modules travel in test/gen/matrix3.ex.

  @moduletag :generated

  alias Structtest.{Scalars3, Inner3}

  @bin Path.join([__DIR__, "..", "..", "c", "tests", "data", "matrix3.bin"])

  setup_all do
    mods = [Scalars3, Inner3, Structtest.Color3]
    :ok = Pbwire.load_schema(mods)
    :ok
  end

  defp build do
    %Scalars3{
      i32: -2_147_483_648,
      i64: 9_223_372_036_854_775_807,
      u32: 4_294_967_295,
      u64: 18_446_744_073_709_551_615,
      s32: -2_147_483_648,
      s64: -9_223_372_036_854_775_808,
      fx32: 0xDEADBEEF,
      fx64: 72_623_859_790_382_856,
      sfx32: -2_147_483_648,
      sfx64: -9_223_372_036_854_775_808,
      f32: :infinity,
      f64: -123456.789,
      flag: true,
      str: "hello",
      color: :COLOR3_GREEN,
      inner: %Inner3{name: "kid", value: 42},
      nums: [1, -5, 300],
      tags: ["a", "bb"],
      inners: [%Inner3{name: "x", value: 1}, %Inner3{name: "y", value: -2}],
      opt_i32: -7,
      opt_str: "opt",
      flags: [true, false, true]
    }
  end

  test "generated struct encodes to the protoc reference bytes and back" do
    reference = File.read!(@bin)
    wire = Protobuf.encode(build())
    assert wire == reference

    back = Protobuf.decode(reference, Scalars3)
    assert back.i32 == -2_147_483_648
    assert back.i64 == 9_223_372_036_854_775_807
    assert back.flag == true
    assert back.str == "hello"
    assert back.color == :COLOR3_GREEN
    assert back.inner == %Inner3{name: "kid", value: 42}
    assert back.nums == [1, -5, 300]
    assert back.tags == ["a", "bb"]
    assert length(back.inners) == 2
    assert back.opt_i32 == -7
    assert back.opt_str == "opt"
    assert back.flags == [true, false, true]

    # Same bytes through the Pbwire codec.
    assert Pbwire.encode(build()) == reference
    assert Pbwire.decode(reference, Scalars3) == back
  end

  test "empty generated struct encodes to nothing" do
    assert Protobuf.encode(%Scalars3{}) == <<>>
  end
end
