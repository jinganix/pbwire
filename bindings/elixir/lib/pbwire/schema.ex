defmodule Pbwire.Schema do
  @moduledoc """
  Turns `elixir-protobuf` generated modules into the schema cache the NIF
  understands.

  This is the Elixir half of the rule the Erlang binding documents: gpb is
  the plugin there (`get_msg_defs/0` in, records out), `elixir-protobuf` is
  the plugin here (`__message_props__/0` in, structs out). There is no
  second wire implementation and no `.proto` parsing in this package: field
  numbers, types, presence and packedness all come from the generated
  `FieldProps`, and the bytes stay in the C core behind `:pbwire`.

  Message and enum names in the generated defs are the Elixir module atoms
  themselves, so two schemas can never collide the way unqualified gpb
  record names can.
  """

  @type field_kind ::
          {:scalar, atom()}
          | {:enum, module()}
          | {:message, module()}
          | {:map, atom(), field_kind()}
          | {:oneof, atom(), [map()]}

  @type field :: %{
          key: atom(),
          fnum: non_neg_integer(),
          rnum: pos_integer(),
          kind: field_kind(),
          occurrence: :required | :optional | :repeated | :defaulty,
          packed?: boolean(),
          default: term()
        }

  @type spec :: %{
          mod: module(),
          name: atom(),
          syntax: :proto2 | :proto3,
          fields: [field()],
          by_key: %{atom() => field()},
          enum_values: %{integer() => atom()} | nil
        }

  @doc """
  Builds the `:pbwire.load_cache/1` defs for `modules` plus every message
  and enum they reference, and the per-module specs `Pbwire` translates
  structs with. Pure: no NIF call, safe to unit test.
  """
  @spec build([module()]) :: {:ok, {list(), %{module() => spec()}}} | {:error, String.t()}
  def build(modules) when is_list(modules) do
    with {:ok, specs} <- collect(modules) do
      {:ok, {defs(specs), specs}}
    end
  end

  @doc "The `:pbwire.set_opts/1` list for the given `Pbwire.load_schema/2` opts."
  @spec nif_opts(keyword()) :: keyword()
  def nif_opts(opts) do
    [with_utf8: Keyword.get(opts, :with_utf8, true)] ++
      if Keyword.has_key?(opts, :string_as_list),
        do: [string_as_list: Keyword.fetch!(opts, :string_as_list)],
        else: []
  end

  # -- collection ------------------------------------------------------

  defp collect(modules) do
    Enum.reduce_while(modules, {:ok, {%{}, []}}, fn mod, {:ok, {specs, queue}} ->
      case collect_module(mod, specs) do
        {:ok, specs, more} -> {:cont, {:ok, {specs, queue ++ more}}}
        {:error, _} = err -> {:halt, err}
      end
    end)
    |> case do
      {:ok, {specs, queue}} -> drain(queue, specs)
      {:error, _} = err -> err
    end
  end

  defp drain([], specs), do: {:ok, specs}

  defp drain([mod | rest], specs) do
    case collect_module(mod, specs) do
      {:ok, specs, more} -> drain(rest ++ more, specs)
      {:error, _} = err -> err
    end
  end

  # Returns {specs, newly referenced modules}; already-seen modules are skipped.
  defp collect_module(mod, specs) do
    if Map.has_key?(specs, mod) do
      {:ok, specs, []}
    else
      with {:ok, props} <- message_props(mod) do
        cond do
          props.map? -> {:ok, specs, []}
          props.enum? -> {:ok, Map.put(specs, mod, enum_spec(mod, props)), []}
          true -> message_spec(mod, props, specs)
        end
      end
    end
  end

  defp message_props(mod) do
    if Code.ensure_loaded?(mod) and function_exported?(mod, :__message_props__, 0) do
      {:ok, mod.__message_props__()}
    else
      {:error, "#{inspect(mod)} is not an elixir-protobuf generated module"}
    end
  rescue
    _ -> {:error, "#{inspect(mod)} is not an elixir-protobuf generated module"}
  end

  defp message_spec(mod, props, specs) do
    syntax = props_syntax(props)

    with {:ok, {fields, refs}} <- message_fields(props, syntax) do
      spec = %{mod: mod, name: mod, syntax: syntax, fields: fields, by_key: %{}, enum_values: nil}
      {:ok, Map.put(specs, mod, %{spec | by_key: Map.new(fields, &{field_key(&1), &1})}), refs}
    end
  end

  defp enum_spec(mod, props) do
    values =
      props.field_props
      |> Enum.map(fn {fnum, fp} -> {fnum, fp.name_atom} end)
      |> Map.new()

    %{
      mod: mod,
      name: mod,
      syntax: props_syntax(props),
      fields: [],
      by_key: %{},
      enum_values: values
    }
  end

  defp props_syntax(props), do: Map.get(props, :syntax, :proto2) || :proto2

  defp field_key(%{kind: {:oneof, group, _}}), do: group
  defp field_key(%{key: key}), do: key

  # -- fields ----------------------------------------------------------

  defp message_fields(props, syntax) do
    oneof_names = Map.new(props.oneof || [], fn {name, index} -> {index, name} end)

    {singles, groups} =
      props.ordered_tags
      |> Enum.map(&Map.fetch!(props.field_props, &1))
      |> Enum.split_with(&is_nil(&1.oneof))

    with {:ok, {singles, refs}} <- translate_fields(singles, syntax),
         {:ok, {oneofs, refs2}} <- translate_oneofs(groups, oneof_names, syntax) do
      entries =
        (singles ++ oneofs)
        |> Enum.sort_by(fn
          %{kind: {:oneof, _, members}} -> members |> Enum.map(& &1.fnum) |> Enum.min()
          %{fnum: fnum} -> fnum
        end)

      {fields, _} =
        Enum.map_reduce(entries, 2, fn
          %{kind: {:oneof, _, members}} = group, rnum ->
            members = Enum.map(members, &%{&1 | rnum: rnum})
            {%{group | rnum: rnum, kind: {:oneof, group.kind |> elem(1), members}}, rnum + 1}

          field, rnum ->
            {%{field | rnum: rnum}, rnum + 1}
        end)

      {:ok, {fields, refs ++ refs2}}
    end
  end

  defp translate_fields(fps, syntax) do
    Enum.reduce_while(fps, {:ok, {[], []}}, fn fp, {:ok, {fields, refs}} ->
      case translate_field(fp, syntax) do
        {:ok, {field, more}} -> {:cont, {:ok, {[field | fields], refs ++ more}}}
        {:error, _} = err -> {:halt, err}
      end
    end)
    |> case do
      {:ok, {fields, refs}} -> {:ok, {Enum.reverse(fields), refs}}
      {:error, _} = err -> err
    end
  end

  defp translate_oneofs(fps, oneof_names, syntax) do
    fps
    |> Enum.group_by(& &1.oneof)
    |> Enum.sort_by(fn {index, _} -> index end)
    |> Enum.reduce_while({:ok, {[], []}}, fn {index, members}, {:ok, {groups, refs}} ->
      group = Map.fetch!(oneof_names, index)

      case translate_fields(members, syntax) do
        {:ok, {members, more}} ->
          field = %{
            key: group,
            fnum: 0,
            rnum: 0,
            kind: {:oneof, group, members},
            occurrence: :optional,
            packed?: false,
            default: nil
          }

          {:cont, {:ok, {[field | groups], refs ++ more}}}

        {:error, _} = err ->
          {:halt, err}
      end
    end)
    |> case do
      {:ok, {groups, refs}} -> {:ok, {Enum.reverse(groups), refs}}
      {:error, _} = err -> err
    end
  end

  defp translate_field(fp, syntax) do
    with {:ok, {kind, refs}} <- field_kind(fp) do
      {:ok,
       {%{
          key: fp.name_atom,
          fnum: fp.fnum,
          rnum: 0,
          kind: kind,
          occurrence: occurrence(fp, syntax),
          packed?: fp.packed?,
          default: fp.default
        }, refs}}
    end
  end

  defp field_kind(%{enum?: true, type: {:enum, mod}}), do: {:ok, {{:enum, mod}, [mod]}}
  defp field_kind(%{map?: true, type: entry}), do: map_kind(entry)

  defp field_kind(%{embedded?: true, type: mod}) when is_atom(mod),
    do: {:ok, {{:message, mod}, [mod]}}

  defp field_kind(%{type: type}) when is_atom(type), do: {:ok, {{:scalar, type}, []}}
  defp field_kind(%{name_atom: key}), do: {:error, "unsupported field type for #{inspect(key)}"}

  defp map_kind(entry) do
    with {:ok, props} <- message_props(entry),
         key_fp = Map.fetch!(props.field_props, 1),
         val_fp = Map.fetch!(props.field_props, 2),
         {:ok, {key_kind, _}} <- scalar_kind(key_fp, "map key"),
         {:ok, {val_kind, refs}} <- value_kind(val_fp) do
      {:ok, {{:map, key_kind, val_kind}, refs}}
    else
      {:error, _} = err -> err
    end
  end

  defp value_kind(%{enum?: true, type: {:enum, mod}}), do: {:ok, {{:enum, mod}, [mod]}}
  defp value_kind(%{map?: true}), do: {:error, "nested maps are not supported"}

  defp value_kind(%{embedded?: true, type: mod}) when is_atom(mod),
    do: {:ok, {{:message, mod}, [mod]}}

  defp value_kind(fp), do: scalar_kind(fp, "map value")

  defp scalar_kind(%{type: type}, _what)
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
              :sfixed64,
              :bool,
              :string,
              :bytes,
              :float,
              :double
            ],
       do: {:ok, {{:scalar, type}, []}}

  defp scalar_kind(%{name_atom: key}, what),
    do: {:error, "unsupported #{what} type for #{inspect(key)}"}

  defp occurrence(%{repeated?: true}, _), do: :repeated
  defp occurrence(%{required?: true}, _), do: :required
  defp occurrence(%{map?: true}, _), do: :repeated
  defp occurrence(%{proto3_optional?: true}, _), do: :optional
  defp occurrence(%{oneof: index}, _syntax) when not is_nil(index), do: :optional
  defp occurrence(_, :proto3), do: :defaulty
  defp occurrence(_, _), do: :optional

  # -- NIF defs --------------------------------------------------------

  defp defs(specs) do
    proto3 = specs |> Map.values() |> Enum.filter(&(&1.syntax == :proto3)) |> Enum.map(& &1.name)

    prefix =
      if proto3 == [] do
        []
      else
        # gpb defs are Erlang terms: the syntax marker is a charlist, not a binary.
        [{:syntax, ~c"proto3"}, {:proto3_msgs, proto3}]
      end

    prefix ++ Enum.flat_map(specs, fn {_mod, spec} -> spec_defs(spec) end)
  end

  defp spec_defs(%{enum_values: values, name: name}) when is_map(values) do
    [{{:enum, name}, Enum.map(values, fn {number, atom} -> {atom, number} end)}]
  end

  defp spec_defs(%{fields: fields, name: name}) do
    [{{:msg, name}, Enum.map(fields, &nif_field/1)}]
  end

  defp nif_field(%{kind: {:oneof, group, members}, rnum: rnum}) do
    {:gpb_oneof, group, rnum, Enum.map(members, &nif_field(%{&1 | rnum: rnum}))}
  end

  defp nif_field(%{
         key: key,
         fnum: fnum,
         rnum: rnum,
         kind: kind,
         occurrence: occ,
         packed?: packed
       }) do
    opts = if packed, do: [:packed], else: []
    {:field, key, fnum, rnum, nif_type(kind), occ, opts}
  end

  defp nif_type({:scalar, type}), do: type
  defp nif_type({:enum, mod}), do: {:enum, mod}
  defp nif_type({:message, mod}), do: {:msg, mod}
  defp nif_type({:map, key, val}), do: {:map, key_scalar(key), nif_map_value(val)}

  defp key_scalar({:scalar, type}), do: type

  defp nif_map_value({:scalar, type}), do: type
  defp nif_map_value({:enum, mod}), do: {:enum, mod}
  defp nif_map_value({:message, mod}), do: {:msg, mod}
end
