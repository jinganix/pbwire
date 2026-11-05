defmodule Pbwire do
  @moduledoc """
  Fast Protobuf encoder/decoder for Elixir: `elixir-protobuf` structs in,
  `elixir-protobuf` structs out, wire bytes identical to the official
  runtime.

  The structs stay exactly as `protoc-gen-elixir` generates them; only the
  codec is swapped. Load the schema once, then encode and decode:

      ok = Pbwire.load_schema([MyApp.Person])
      bin = Pbwire.encode(%MyApp.Person{name: "alice", id: 1})
      %MyApp.Person{} = Pbwire.decode(bin, MyApp.Person)

  Underneath, `Pbwire.Schema` translates the generated
  `__message_props__/0` into the defs the `:pbwire` Erlang NIF consumes
  (the same NIF and C core as `bindings/erlang`), and this module translates
  structs to the record tuples the NIF encodes and back. No field numbers,
  wire types or presence rules are re-derived here: they all come from the
  generated modules.

  The schema cache is global to the BEAM node (it lives in the NIF), so one
  VM holds one schema: two applications with different `.proto` schemas
  need separate nodes, exactly like the Erlang binding.
  """

  alias Pbwire.Schema

  @specs_key {Schema, :specs}

  @doc """
  Loads the schema for `modules` (plus every message and enum they
  reference), replacing any schema loaded before.

  Options are passed to `:pbwire.set_opts/1` before loading:

    * `:with_utf8` (default `true`) — accept non-Latin-1 UTF-8 in string
      fields when encoding. Elixir strings are UTF-8, so this stays on
      unless you need the Erlang default (`false`).
    * `:string_as_list` — when `true`, decode string fields as charlists
      instead of binaries. Leave it off: Elixir strings are binaries.

  Raises `ArgumentError` when a module is not an `elixir-protobuf` generated
  module or carries a field this binding does not cover.
  """
  @spec load_schema([module()], keyword()) :: :ok
  def load_schema(modules, opts \\ []) when is_list(modules) do
    case Schema.build(modules) do
      {:ok, {defs, specs}} ->
        :ok = :pbwire.set_opts(Schema.nif_opts(opts))
        :ok = :pbwire.load_cache(defs)
        :persistent_term.put(@specs_key, specs)
        :ok

      {:error, reason} ->
        raise ArgumentError, "Pbwire.load_schema/2: #{reason}"
    end
  end

  @doc """
  Forgets the loaded schema. Later `encode/1` / `decode/2` calls raise until
  `load_schema/2` runs again.
  """
  @spec purge_schema() :: :ok
  def purge_schema do
    :persistent_term.erase(@specs_key)
    :pbwire.purge_cache()
  end

  @doc """
  Encodes a struct to protobuf wire format, byte-identical to
  `Protobuf.encode/1`.

  Raises when no schema is loaded for the struct's module, or when a value
  does not fit its field (for example a missing `required` field).
  """
  @spec encode(struct()) :: binary()
  def encode(%mod{} = struct) do
    :pbwire.encode(to_tuple(fetch!(mod), struct))
  end

  @doc """
  Decodes wire bytes into a struct, equal to
  `Protobuf.Decoder.decode(bin, module, [])`.

  Unknown fields are dropped (the tuple form the NIF decodes to has no
  storage for them, same as the Erlang binding); the decoded struct carries
  `__unknown_fields__: []`.
  """
  @spec decode(binary(), module()) :: struct()
  def decode(binary, module) when is_binary(binary) and is_atom(module) do
    from_tuple(fetch!(module), :pbwire.decode(binary, module))
  end

  @doc "`load_schema/2` followed by `encode/1`."
  @spec encode_msg(struct(), [module()]) :: binary()
  def encode_msg(%_{} = struct, modules) when is_list(modules) do
    :ok = load_schema(modules)
    encode(struct)
  end

  @doc "`load_schema/2` followed by `decode/2`."
  @spec decode_msg(binary(), module(), [module()]) :: struct()
  def decode_msg(binary, module, modules) when is_list(modules) do
    :ok = load_schema(modules ++ [module])
    decode(binary, module)
  end

  # -- struct -> tuple -------------------------------------------------

  defp fetch!(mod) do
    case :persistent_term.get(@specs_key, %{}) do
      %{^mod => spec} ->
        spec

      _ ->
        raise ArgumentError,
              "Pbwire: no schema loaded for #{inspect(mod)}, call Pbwire.load_schema/1 first"
    end
  end

  defp to_tuple(%{name: name, fields: fields} = spec, struct) do
    values = Enum.map(fields, &encode_field(&1, Map.fetch!(struct, field_key(&1)), spec))
    List.to_tuple([name | values])
  end

  defp field_key(%{kind: {:oneof, group, _}}), do: group
  defp field_key(%{key: key}), do: key

  defp encode_field(%{occurrence: :repeated, kind: {:map, _, val_kind}}, value, spec) do
    items = if is_map(value), do: Map.to_list(value), else: value || []
    Enum.map(items, fn {k, v} -> {encode_map_key(k), encode_map_value(val_kind, v, spec)} end)
  end

  defp encode_field(%{occurrence: :repeated, kind: kind}, value, spec) do
    Enum.map(value || [], &encode_value(kind, &1, spec))
  end

  defp encode_field(%{kind: {:oneof, _, _}}, nil, _spec), do: :undefined

  defp encode_field(%{kind: {:oneof, _, members}}, {member, value}, spec) do
    case Enum.find(members, &(&1.key == member)) do
      nil -> raise ArgumentError, "Pbwire.encode/1: unknown oneof member #{inspect(member)}"
      %{kind: kind} -> {member, encode_value(kind, value, spec)}
    end
  end

  defp encode_field(%{kind: {:oneof, _, _}}, value, _spec) do
    raise ArgumentError,
          "Pbwire.encode/1: oneof value must be nil or {member, value}, got #{inspect(value)}"
  end

  # proto2 omission rule, mirroring elixir-protobuf: a singular optional
  # field holding its custom default is not written (implicit zeros are
  # still written; required fields always are).
  defp encode_field(%{occurrence: :optional, kind: kind, default: default} = field, value, spec)
       when not is_nil(default) and (elem(kind, 0) == :scalar or elem(kind, 0) == :enum) do
    if value == default, do: :undefined, else: encode_value(field.kind, value, spec)
  end

  defp encode_field(%{kind: kind}, value, spec), do: encode_value(kind, value, spec)

  defp encode_value({:scalar, type}, nil, _spec), do: scalar_absent(type)
  defp encode_value({:scalar, type}, value, _spec), do: encode_scalar(type, value)
  defp encode_value({:enum, _}, nil, _spec), do: :undefined
  defp encode_value({:enum, _}, value, _spec), do: value
  defp encode_value({:message, _}, nil, _spec), do: :undefined
  defp encode_value({:message, mod}, %_{} = value, _spec), do: to_tuple(fetch!(mod), value)

  # The NIF skips `:undefined` for singular optional/defaulty/oneof fields.
  defp scalar_absent(_type), do: :undefined

  defp encode_map_key([]), do: ""
  defp encode_map_key(key), do: key

  defp encode_map_value({:message, mod}, %_{} = value, _spec), do: to_tuple(fetch!(mod), value)
  defp encode_map_value({:scalar, type}, value, _spec), do: encode_scalar(type, value)
  defp encode_map_value({:enum, _}, value, _spec), do: value

  defp encode_map_value({:map, _, inner_val}, value, spec) do
    items = if is_map(value), do: Map.to_list(value), else: value || []
    Enum.map(items, fn {k, v} -> {encode_map_key(k), encode_map_value(inner_val, v, spec)} end)
  end

  defp encode_scalar(:string, value) when is_binary(value) or is_list(value), do: value
  defp encode_scalar(:bytes, value) when is_binary(value) or is_list(value), do: value
  defp encode_scalar(type, :negative_infinity) when type in [:float, :double], do: :"-infinity"
  defp encode_scalar(_type, value), do: value

  # -- tuple -> struct -------------------------------------------------

  defp from_tuple(%{mod: mod, fields: fields} = spec, tuple) do
    [_ | values] = Tuple.to_list(tuple)

    pairs =
      fields
      |> Enum.zip(values)
      |> Enum.map(fn {field, value} -> {field_key(field), decode_field(field, value, spec)} end)

    struct(mod, pairs ++ [__unknown_fields__: []])
  end

  defp decode_field(%{kind: {:oneof, _, _}}, :undefined, _spec), do: nil

  defp decode_field(%{kind: {:oneof, _, members}}, {member, value}, spec) do
    case Enum.find(members, &(&1.key == member)) do
      nil -> {member, value}
      %{kind: kind} -> {member, decode_value(kind, value, spec)}
    end
  end

  defp decode_field(%{occurrence: :repeated, kind: {:map, _, val_kind}}, values, spec)
       when is_list(values) do
    Map.new(values, fn {k, v} -> {decode_map_key(k), decode_map_value(val_kind, v, spec)} end)
  end

  defp decode_field(%{occurrence: :repeated, kind: kind}, values, spec) when is_list(values) do
    Enum.map(values, &decode_value(kind, &1, spec))
  end

  defp decode_field(%{kind: _} = field, :undefined, spec), do: absent(field, spec)
  defp decode_field(%{kind: kind}, value, spec), do: decode_value(kind, value, spec)

  defp decode_value({:scalar, :string}, [], _spec), do: ""

  defp decode_value({:scalar, type}, :"-infinity", _spec) when type in [:float, :double],
    do: :negative_infinity

  defp decode_value({:scalar, _}, value, _spec), do: value
  defp decode_value({:enum, _}, value, _spec), do: value

  defp decode_value({:message, mod}, tuple, _spec) when is_tuple(tuple),
    do: from_tuple(fetch!(mod), tuple)

  defp decode_map_key([]), do: ""
  defp decode_map_key(key), do: key

  defp decode_map_value({:message, mod}, tuple, _spec) when is_tuple(tuple),
    do: from_tuple(fetch!(mod), tuple)

  defp decode_map_value({:scalar, :string}, [], _spec), do: ""

  defp decode_map_value({:scalar, type}, :"-infinity", _spec) when type in [:float, :double],
    do: :negative_infinity

  defp decode_map_value({:map, _, inner_val}, values, spec) when is_list(values) do
    Map.new(values, fn {k, v} -> {decode_map_key(k), decode_map_value(inner_val, v, spec)} end)
  end

  defp decode_map_value(_kind, value, _spec), do: value

  # What an absent field decodes to. The NIF already materializes repeated
  # (`[]`), proto3 implicit-presence zeros and map-entry defaults; only the
  # `:undefined` cases need Elixir-side defaults.
  defp absent(%{kind: {:map, _, _}}, _spec), do: %{}
  defp absent(%{kind: {:message, _}}, _spec), do: nil
  defp absent(%{kind: {:enum, _}}, _spec), do: nil
  defp absent(%{occurrence: :repeated}, _spec), do: []
  defp absent(%{occurrence: :required}, _spec), do: nil
  defp absent(%{occurrence: :defaulty, kind: {:scalar, type}}, _spec), do: scalar_zero(type, nil)

  defp absent(%{kind: {:scalar, _}, default: default}, _spec) when not is_nil(default),
    do: default

  defp absent(_field, _spec), do: nil

  defp scalar_zero(type, _)
       when type in [
              :int32,
              :int64,
              :uint32,
              :uint64,
              :sint32,
              :sint64,
              :fixed32,
              :fixed64,
              :sfixed32,
              :sfixed64
            ],
       do: 0

  defp scalar_zero(:bool, _), do: false
  defp scalar_zero(type, _) when type in [:float, :double], do: 0.0
  defp scalar_zero(:string, _), do: ""
  defp scalar_zero(:bytes, _), do: <<>>
end
