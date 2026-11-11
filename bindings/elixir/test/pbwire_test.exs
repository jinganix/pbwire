defmodule PbwireTest do
  @moduledoc """
  Self-check: every encode must be byte-identical to `Protobuf.encode/1`
  and every decode must equal `Protobuf.decode/2` on the same bytes.
  `elixir-protobuf` is the oracle, the way gpb is the oracle for the
  Erlang binding's suite.
  """
  use ExUnit.Case, async: false

  alias Pbwire.Test.Addressbook.{Person, PhoneNumber, PhoneType}
  alias Pbwire.Test.{Doc, DocKind, Scalars}
  alias Pbwire.Test.Nested.Doc, as: NestedDoc

  @mods [Person, PhoneNumber, PhoneType, Scalars, Doc, DocKind, NestedDoc]

  setup do
    :ok = Pbwire.load_schema(@mods)
    :ok
  end

  # -- loading ---------------------------------------------------------

  test "load_schema rejects non-protobuf modules" do
    assert_raise ArgumentError, ~r/not an elixir-protobuf/, fn ->
      Pbwire.load_schema([Enum])
    end
  end

  test "encode without a schema raises" do
    Pbwire.purge_schema()

    assert_raise ArgumentError, ~r/no schema loaded/, fn ->
      Pbwire.encode(%Person{name: "a", id: 1})
    end
  after
    Pbwire.load_schema(@mods)
  end

  test "decode of an unknown message raises" do
    assert_raise ArgumentError, ~r/no schema loaded/, fn ->
      Pbwire.decode(<<>>, NotLoaded)
    end
  end

  # -- proto2 addressbook ----------------------------------------------

  test "addressbook round trip, byte-identical to elixir-protobuf" do
    msg = %Person{
      name: "alice",
      id: 1234,
      email: "a@example.com",
      phones: [%PhoneNumber{number: "555-1234", type: :MOBILE}],
      nickname: "ally"
    }

    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, Person) == msg
    assert Pbwire.decode(bin, Person) == Protobuf.decode(bin, Person)
  end

  test "proto2 unset optional decodes to nil, custom default applies" do
    msg = %Person{name: "bob", id: 7}
    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)

    decoded = Pbwire.decode(bin, Person)
    assert decoded.email == nil
    assert decoded.phones == []
    # elixir-protobuf applies [default = "anon"] on decode; so do we.
    assert decoded.nickname == Protobuf.decode(bin, Person).nickname
  end

  test "proto2 required fields are enforced" do
    assert_raise ErlangError, fn ->
      Pbwire.encode(%Person{name: "no-id"})
    end
  end

  # -- proto2 scalars ---------------------------------------------------

  test "every scalar type round trips, packed and unpacked" do
    msg = %Scalars{
      f_int32: -1,
      f_int64: -9_000_000_000,
      f_uint32: 4_000_000_000,
      f_uint64: 18_000_000_000_000_000_000,
      f_sint32: -2_000_000_000,
      f_sint64: -4_000_000_000_000_000_000,
      f_fixed32: 4_000_000_001,
      f_fixed64: 18_000_000_000_000_000_001,
      f_sfixed32: -2_000_000_001,
      f_sfixed64: -4_000_000_000_000_000_001,
      f_bool: true,
      f_float: 1.5,
      f_double: -1.25,
      f_string: "héllo",
      f_bytes: <<0, 255, 1>>,
      r_packed: [1, -2, 300],
      r_unpacked: [1, -2, 300]
    }

    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, Scalars) == Protobuf.decode(bin, Scalars)
  end

  test "float specials map between the two runtimes' atoms" do
    for special <- [:infinity, :negative_infinity] do
      msg = %Scalars{f_float: special, f_double: special}
      bin = Pbwire.encode(msg)
      assert bin == Protobuf.encode(msg)
      assert Pbwire.decode(bin, Scalars) == Protobuf.decode(bin, Scalars)
    end

    # NaN has several valid encodings (the NIF writes the quiet-NaN bits,
    # elixir-protobuf a signalling variant); both decode to :nan.
    msg = %Scalars{f_float: :nan, f_double: :nan}

    assert Pbwire.decode(Pbwire.encode(msg), Scalars) ==
             Protobuf.decode(Protobuf.encode(msg), Scalars)
  end

  # -- proto3 doc -------------------------------------------------------

  test "proto3 zero values are omitted, byte-identical" do
    msg = %Doc{}
    bin = Pbwire.encode(msg)
    assert bin == <<>>
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, Doc) == Protobuf.decode(bin, Doc)
  end

  test "proto3 full message round trip" do
    sub = %Person{name: "nested", id: 3, phones: [%PhoneNumber{number: "x"}]}

    msg = %Doc{
      title: "t",
      count: -42,
      flag: true,
      blob: <<1, 2, 3>>,
      score: 2.5,
      ratio: 0.5,
      kind: :B,
      sub: sub,
      labels: ["a", "b"],
      nums: [1, 2, 3],
      attrs: %{"k" => 1},
      counts: %{7 => %Doc{title: "inner"}},
      choice: {:pick_b, 9}
    }

    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, Doc) == Protobuf.decode(bin, Doc)
    assert Pbwire.decode(bin, Doc) == msg
  end

  test "nested maps round trip, byte-identical to elixir-protobuf" do
    msg = %NestedDoc{nested: %{"a" => %{"x" => 1, "y" => 2}, "b" => %{}}}

    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, NestedDoc) == Protobuf.decode(bin, NestedDoc)
    assert Pbwire.decode(bin, NestedDoc).nested == msg.nested
    assert Pbwire.encode(Pbwire.decode(bin, NestedDoc)) == bin
  end

  test "proto3 explicit optional keeps presence of zero" do
    msg = %Doc{comment: "", level: 0}
    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert byte_size(bin) > 0
    assert Pbwire.decode(bin, Doc) == msg
  end

  test "oneof members round trip, each member type" do
    for choice <- [nil, {:pick_a, "s"}, {:pick_b, -5}, {:pick_m, %Person{name: "n", id: 1}}] do
      msg = %Doc{title: "oneof", choice: choice}
      bin = Pbwire.encode(msg)
      assert bin == Protobuf.encode(msg), "choice #{inspect(choice)}"
      assert Pbwire.decode(bin, Doc) == Protobuf.decode(bin, Doc)
    end
  end

  test "unknown enum numbers decode to integers like the oracle" do
    # field 7 (kind), varint 99: unknown to DocKind.
    bin = <<56, 99>>
    assert Pbwire.decode(bin, Doc) == Protobuf.decode(bin, Doc)
    assert Pbwire.decode(bin, Doc).kind == 99
  end

  test "malformed wire data raises instead of returning garbage" do
    assert_raise ErlangError, fn -> Pbwire.decode(<<255, 255, 255>>, Doc) end
    assert_raise ErlangError, fn -> Pbwire.decode(<<10, 5, 1, 2>>, Doc) end
  end

  test "loading a second schema replaces the first" do
    :ok = Pbwire.load_schema([Scalars])

    assert_raise ArgumentError, ~r/no schema loaded/, fn ->
      Pbwire.encode(%Person{name: "a", id: 1})
    end

    msg = %Scalars{f_int32: 1}
    assert Pbwire.encode(msg) == Protobuf.encode(msg)
  after
    Pbwire.load_schema(@mods)
  end

  test "large repeated field round trip" do
    msg = %Doc{nums: Enum.to_list(1..3000), labels: List.duplicate("x", 100)}
    bin = Pbwire.encode(msg)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode(bin, Doc) == msg
  end

  # -- convenience wrappers ---------------------------------------------

  test "encode_msg/decode_msg load the schema per call" do
    Pbwire.purge_schema()
    msg = %Person{name: "w", id: 9}
    bin = Pbwire.encode_msg(msg, @mods)
    assert bin == Protobuf.encode(msg)
    assert Pbwire.decode_msg(bin, Person, @mods) == msg
  after
    Pbwire.load_schema(@mods)
  end

  # -- schema defs (no NIF needed) --------------------------------------

  test "generated defs carry field numbers, presence and packing" do
    {:ok, {defs, _specs}} = Pbwire.Schema.build([Doc, DocKind])

    assert {:syntax, ~c"proto3"} in defs
    assert {:proto3_msgs, proto3} = List.keyfind(defs, :proto3_msgs, 0)
    assert Doc in proto3 and DocKind in proto3

    {{:msg, Doc}, fields} = List.keyfind(defs, {:msg, Doc}, 0)

    by_name =
      Map.new(fields, fn
        {:field, name, _, _, _, _, _} = f -> {name, f}
        {:gpb_oneof, name, _, _} = f -> {name, f}
      end)

    assert {:field, :title, 1, _, :string, :defaulty, []} = by_name[:title]
    assert {:field, :comment, 13, _, :string, :optional, []} = by_name[:comment]
    assert {:field, :nums, 10, _, :int32, :repeated, [:packed]} = by_name[:nums]
    assert {:field, :kind, 7, _, {:enum, DocKind}, :defaulty, []} = by_name[:kind]
    assert {:field, :attrs, 11, _, {:map, :string, :int32}, :repeated, []} = by_name[:attrs]
    {:ok, {nested_defs, _}} = Pbwire.Schema.build([NestedDoc])
    {{:msg, NestedDoc}, [nested_field]} = List.keyfind(nested_defs, {:msg, NestedDoc}, 0)

    assert {:field, :nested, 1, _, {:map, :string, {:map, :string, :int32}}, :repeated, []} =
             nested_field

    assert {:gpb_oneof, :choice, _, members} = by_name[:choice]
    assert Enum.map(members, &elem(&1, 1)) == [:pick_a, :pick_b, :pick_m]
  end

  # -- delimited framing ------------------------------------------------

  test "delimited round trip, frame plus rest" do
    msg = %Person{name: "alice", id: 1}
    bin = Pbwire.encode(msg)
    frame = Pbwire.encode_delimited(msg)
    assert frame == <<byte_size(bin)>> <> bin
    assert Pbwire.decode_delimited(frame, Person) == {Pbwire.decode(bin, Person), <<>>}
  end

  test "delimited concatenated frames decode in turn" do
    m1 = %Person{name: "a", id: 1}
    m2 = %Person{name: "b", id: 2, email: "x@y"}
    stream = Pbwire.encode_delimited(m1) <> Pbwire.encode_delimited(m2)
    assert {^m1, rest} = Pbwire.decode_delimited(stream, Person)
    assert Pbwire.decode_delimited(rest, Person) == {m2, <<>>}
  end

  test "delimited truncated input raises" do
    assert_raise ErlangError, fn -> Pbwire.decode_delimited(<<5, 1, 2>>, Person) end
    assert_raise ErlangError, fn -> Pbwire.decode_delimited(<<>>, Person) end
  end

  # -- deterministic encoding -------------------------------------------

  test "deterministic map order is insertion-independent" do
    a = %Doc{title: "det", attrs: %{"b" => 2, "a" => 1}, counts: %{3 => %Doc{}, 1 => %Doc{}}}
    b = %Doc{title: "det", attrs: %{"a" => 1, "b" => 2}, counts: %{1 => %Doc{}, 3 => %Doc{}}}
    assert Pbwire.encode_deterministic(a) == Pbwire.encode_deterministic(b)

    assert Pbwire.decode(Pbwire.encode_deterministic(a), Doc) ==
             Protobuf.decode(Protobuf.encode(a), Doc)
  end

  test "deterministic keeps repeated order and matches the oracle without maps" do
    msg = %Doc{title: "t", nums: [3, 1, 2], labels: ["b", "a"]}
    assert Pbwire.encode_deterministic(msg) == Protobuf.encode(msg)
    assert Pbwire.decode(Pbwire.encode_deterministic(msg), Doc) == msg
  end

  # -- limits ------------------------------------------------------------

  test "max_bytes rejects oversized inputs" do
    msg = %Person{name: "alice", id: 1}
    bin = Pbwire.encode(msg)
    size = byte_size(bin)

    :ok = Pbwire.load_schema(@mods, max_bytes: size - 1)
    assert_raise ErlangError, fn -> Pbwire.decode(bin, Person) end

    :ok = Pbwire.load_schema(@mods, max_bytes: size)
    assert Pbwire.decode(bin, Person) == msg
  after
    Pbwire.load_schema(@mods, max_bytes: 0)
  end

  test "max_depth rejects deep nesting" do
    deep =
      Enum.reduce(1..8, %Doc{}, fn _, acc -> %Doc{counts: %{1 => acc}} end)

    bin = Pbwire.encode(deep)
    assert Pbwire.decode(bin, Doc) == deep

    :ok = Pbwire.load_schema(@mods, max_depth: 3)
    assert_raise ErlangError, fn -> Pbwire.decode(bin, Doc) end
  after
    Pbwire.load_schema(@mods, max_depth: 0)
  end
end
