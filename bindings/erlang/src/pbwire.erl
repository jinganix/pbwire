%% @doc Fast Google Protobuf (proto2/proto3) encoder and decoder for Erlang/OTP, written in C as a
%% NIF.
%%
%% Drop-in replacement for the `encode_msg/1' and `decode_msg/2' functions that
%% <a href="https://github.com/tomas-abrahamsson/gpb">gpb</a> generates from `.proto' files: same record tuples,
%% same wire format, byte-identical output, no libprotobuf or protoc dependency.
%%
%% Load a schema once with {@link load_cache/1}, then call {@link encode/1} and {@link decode/2}:
%%
%% ```
%% ok = pbwire:load_cache(x:get_msg_defs()),   %% gpb 5+: x:get_proto_defs()
%% Bin = pbwire:encode(#'Person'{name = "abc", id = 345}),
%% #'Person'{} = pbwire:decode(Bin, 'Person').
%% '''
%%
%% The schema cache is global to the BEAM node and shared by all processes; encode and decode take a
%% read lock and are safe to call concurrently. Calls run on normal schedulers and are not dirty NIFs.
%%
%% Only `{string_as_list, boolean()}', `{with_utf8, boolean()}' and the numeric guards exist, see
%% {@link set_opts/1}.
-module(pbwire).

-export([
    set_opts/1,
    load_cache/1,
    purge_cache/0,
    encode/1,
    encode_msg/2,
    encode_delimited/1,
    encode_deterministic/2,
    decode/2,
    decode_msg/3,
    decode_delimited/2,
    debug_term/1
]).

-define(NOT_LOADED, not_loaded(?LINE)).

-compile([no_native]).

-on_load(init/0).

init() ->
    PrivDir = case code:priv_dir(?MODULE) of
        {error, _} ->
            BeamDir = filename:dirname(code:which(?MODULE)),
            AppPath = filename:dirname(BeamDir),
            filename:join(AppPath, "priv");
        Path ->
            Path
    end,
    %% One state slot per thread that can run a NIF: dirty CPU schedulers
    %% included, so two threads never share a slot.
    Threads = erlang:system_info(schedulers) + erlang:system_info(dirty_cpu_schedulers),
    ok = erlang:load_nif(filename:join(PrivDir, "pbwire"), Threads).

not_loaded(Line) ->
    erlang:nif_error({not_loaded, [{module, ?MODULE}, {line, Line}]}).

%% @doc Set encoder/decoder options. Call before {@link load_cache/1}; the options apply to the
%% schema loaded afterwards.
%%
%% `{string_as_list, true}' makes string fields decode to lists instead of binaries (default
%% `false'). `{with_utf8, true}' allows non-Latin-1 UTF-8 in string fields when encoding (default
%% `false'). `{retain_cap, Bytes}' caps the resident encode buffer per scheduler (like the C
%% core's `pbw_ctx_set_retain_cap', same 1 MiB default when unset, 0 keeps nothing); it applies
%% to live scheduler contexts as well as future ones.
%%
%% `{max_bytes, Bytes}' rejects `decode/2' inputs larger than `Bytes', like the C core's
%% `pbw_limits.max_bytes'. `{max_depth, Levels}' rejects `decode/2' inputs nested deeper than
%% `Levels' messages, like `pbw_limits.max_depth' (one singular-nested message costs one frame,
%% a repeated-nested one costs two; unknown groups are additionally capped by the smaller of
%% `Levels' and the built-in group cap). Both are 0 (unset) by default; neither affects encoding.
%%
%% Raises `badarg' for unknown options, bad values, or an improper list.
-spec set_opts([{string_as_list, boolean()} | {with_utf8, boolean()} |
                 {retain_cap, non_neg_integer()} | {max_bytes, non_neg_integer()} |
                 {max_depth, non_neg_integer()}]) -> ok.
set_opts(_Opts) ->
    ?NOT_LOADED.

%% @doc Load a schema, replacing any schema loaded before. `Defs' is exactly what a gpb generated
%% module returns from `Module:get_msg_defs()', or `Module:get_proto_defs()' on gpb 5+: the latter
%% also carries the proto3 markers, which proto3 messages need to omit zero-valued singular fields.
%%
%% The cache is global to the node, so all processes and applications on one VM share it; two
%% applications with different schemas need separate nodes or one coordinated `load_cache/1' call.
-spec load_cache(list()) -> ok.
load_cache(_List) ->
    ?NOT_LOADED.

%% @doc Free the loaded schema. Later calls to {@link encode/1} or {@link decode/2} raise
%% `cache_not_exists' until {@link load_cache/1} is called again.
-spec purge_cache() -> ok.
purge_cache() ->
    ?NOT_LOADED.

%% @doc Encode a message. `Msg' is a gpb record (or a map tuple from a `-maps' generated module) that
%% matches a message in the loaded schema; the result is protobuf wire format.
%%
%% Raises `badarg' if `Msg' is not a tuple or does not match the schema, and `cache_not_exists' if no
%% schema is loaded.
-spec encode(tuple()) -> binary().
encode(_Tuple) ->
    ?NOT_LOADED.

%% @doc Decode binary protobuf wire format into a message record or map tuple. `Name' is the message
%% atom as written in the `.proto' file, e.g. `'Person''.
%%
%% Raises `cache_not_exists' when no schema is loaded. An unknown `Name' raises that atom as the
%% exception reason (not the fixed atom `unknown_msg'), and malformed wire data raises the input
%% binary; both match the upstream `:pbwire` NIF's behaviour.
-spec decode(binary(), atom()) -> tuple().
decode(_Binary, _Name) ->
    ?NOT_LOADED.

%% @doc Print the internal node layout of a loaded schema entry. Debugging aid that returns its
%% argument unchanged.
-spec debug_term(term()) -> term().
debug_term(_Term) ->
    ?NOT_LOADED.

%% @doc {@link load_cache/1} followed by {@link encode/1}, for call sites where loading the schema on
%% every call is acceptable.
-spec encode_msg(tuple(), list()) -> binary().
encode_msg(Msg, Defs) ->
    ok = load_cache(Defs),
    encode(Msg).

%% @doc {@link load_cache/1} followed by {@link decode/2}, for call sites where loading the schema on
%% every call is acceptable.
-spec decode_msg(binary(), atom(), list()) -> tuple().
decode_msg(Bin, Name, Defs) ->
    ok = load_cache(Defs),
    decode(Bin, Name).

%% @doc Encode a message with length-delimited framing: a varint length prefix followed by the
%% message bytes (what the C core's `pbw_encode_delimited' writes, the standard framing for a
%% stream of messages). Decodes with {@link decode_delimited/2}.
%%
%% Raises like {@link encode/1}.
-spec encode_delimited(tuple()) -> binary().
encode_delimited(Msg) ->
    Bin = encode(Msg),
    <<(encode_varint(byte_size(Bin)))/binary, Bin/binary>>.

%% @doc Decode one length-delimited frame (see {@link encode_delimited/1}). Returns the message
%% and the remaining bytes, so concatenated frames decode with repeated calls.
%%
%% A truncated prefix or frame raises the input binary, like malformed wire data in {@link
%% decode/2}; other failures raise like {@link decode/2}.
-spec decode_delimited(binary(), atom()) -> {tuple(), binary()}.
decode_delimited(Bin, Name) when is_binary(Bin), is_atom(Name) ->
    {Len, Rest} = decode_varint(Bin, Bin),
    case Rest of
        <<Frame:Len/binary, Rest2/binary>> ->
            {decode(Frame, Name), Rest2};
        _ ->
            erlang:error(Bin)
    end;
decode_delimited(_, _) ->
    erlang:error(badarg).

%% @doc Encode a message with map entries sorted by key, so equal messages encode to equal bytes
%% regardless of insertion order (what the C core's `pbw_encode_deterministic' does for maps;
%% non-map fields encode exactly as {@link encode/1} writes them, so repeated-field order is
%% kept). Key order matches the core: strings compare byte-wise, signed integer keys compare
%% signed, every other key (unsigned and fixed-width integers, booleans) compares unsigned.
%%
%% `Defs' is the same list {@link load_cache/1} takes: the cache is loaded like {@link
%% encode_msg/2} does, then the message is sorted against it before encoding.
%%
%% Raises like {@link encode_msg/2}.
-spec encode_deterministic(tuple(), list()) -> binary().
encode_deterministic(Msg, Defs) ->
    ok = load_cache(Defs),
    encode(sort_deterministic(Msg, msg_index(Defs))).

%% -- length-delimited framing ------------------------------------------

%% Unsigned varint32, as the wire writes lengths.
encode_varint(N) when is_integer(N), N >= 0 ->
    encode_varint(N, <<>>).

encode_varint(N, Acc) when N < 128 ->
    <<Acc/binary, N>>;
encode_varint(N, Acc) ->
    encode_varint(N bsr 7, <<Acc/binary, ((N band 127) bor 128)>>).

%% Strict varint32 like the core's length reader: at most five bytes, nothing
%% above bit 31. Anything else raises the whole input, like malformed data.
decode_varint(Bin, Orig) ->
    decode_varint(Bin, 0, 0, Orig).

decode_varint(Bin, Shift, Acc, Orig) when Shift < 28 ->
    case Bin of
        <<Byte, Rest/binary>> ->
            Val = Acc bor ((Byte band 127) bsl Shift),
            if
                Byte band 128 =:= 0 -> {Val, Rest};
                true -> decode_varint(Rest, Shift + 7, Val, Orig)
            end;
        _ ->
            erlang:error(Orig)
    end;
decode_varint(Bin, 28, Acc, Orig) ->
    case Bin of
        <<Byte, Rest/binary>> when Byte < 16 ->
            {Acc bor (Byte bsl 28), Rest};
        _ ->
            erlang:error(Orig)
    end.

%% -- deterministic map ordering ----------------------------------------

%% Message name to the field facts the sort needs: record position plus a
%% type small enough to sort by. Tuples stay positional (no gpb header needed).
msg_index(Defs) ->
    lists:foldl(fun index_def/2, #{}, Defs).

index_def({{msg, Name}, Fields}, Acc) when is_list(Fields) ->
    Acc#{Name => [index_field(F) || F <- Fields]};
index_def(_, Acc) ->
    Acc.

index_field({field, _Name, _FNum, RNum, Type, Occ, _Opts}) ->
    {RNum, index_type(Type), Occ};
index_field({gpb_oneof, _Name, RNum, Subs}) ->
    {RNum, {oneof, maps:from_list([{M, {index_type(T), O}} || {field, M, _F, _R, T, O, _Op} <- Subs])}, optional}.

index_type({map, K, V}) ->
    {map, K, index_map_value(V)};
index_type({msg, _} = T) ->
    T;
index_type({group, _} = T) ->
    T;
index_type(_) ->
    scalar.

index_map_value({map, K, V}) ->
    {map, K, index_map_value(V)};
index_map_value({msg, _} = T) ->
    T;
index_map_value(_) ->
    scalar.

%% Sort map entries everywhere below this message; leave anything the index
%% does not know (map-entry tuples carry no name) for the encoder to reject.
sort_deterministic(Msg, Idx) when is_tuple(Msg) ->
    case maps:find(element(1, Msg), Idx) of
        {ok, Fields} -> sort_tuple(Msg, Fields, Idx);
        error -> Msg
    end;
sort_deterministic(Msg, _Idx) ->
    Msg.

sort_tuple(Msg, Fields, Idx) ->
    lists:foldl(fun(F, M) -> sort_field(M, F, Idx) end, Msg, Fields).

sort_field(Msg, {RNum, {map, KType, VType}, _Occ}, Idx) ->
    case element(RNum, Msg) of
        V when is_list(V) -> setelement(RNum, Msg, sort_entries(V, KType, VType, Idx));
        _ -> Msg
    end;
sort_field(Msg, {RNum, {msg, _}, repeated}, Idx) ->
    case element(RNum, Msg) of
        V when is_list(V) -> setelement(RNum, Msg, [sort_deterministic(E, Idx) || E <- V]);
        _ -> Msg
    end;
sort_field(Msg, {RNum, {group, _}, repeated}, Idx) ->
    case element(RNum, Msg) of
        V when is_list(V) -> setelement(RNum, Msg, [sort_deterministic(E, Idx) || E <- V]);
        _ -> Msg
    end;
sort_field(Msg, {RNum, {msg, _}, _Occ}, Idx) ->
    case element(RNum, Msg) of
        V when is_tuple(V) -> setelement(RNum, Msg, sort_deterministic(V, Idx));
        _ -> Msg
    end;
sort_field(Msg, {RNum, {group, _}, _Occ}, Idx) ->
    case element(RNum, Msg) of
        V when is_tuple(V) -> setelement(RNum, Msg, sort_deterministic(V, Idx));
        _ -> Msg
    end;
sort_field(Msg, {RNum, {oneof, Members}, _Occ}, Idx) ->
    case element(RNum, Msg) of
        {Member, Val} ->
            case maps:find(Member, Members) of
                {ok, {Kind, _}} -> setelement(RNum, Msg, {Member, sort_value(Val, Kind, Idx)});
                error -> Msg
            end;
        _ -> Msg
    end;
sort_field(Msg, _Field, _Idx) ->
    Msg.

sort_value(V, {map, KType, VType}, Idx) when is_list(V) ->
    sort_entries(V, KType, VType, Idx);
sort_value(V, {msg, _}, Idx) when is_tuple(V) ->
    sort_deterministic(V, Idx);
sort_value(V, {group, _}, Idx) when is_tuple(V) ->
    sort_deterministic(V, Idx);
sort_value(V, _, _) ->
    V.

sort_map_value(V, {map, KType, VType}, Idx) when is_list(V) ->
    sort_entries(V, KType, VType, Idx);
sort_map_value(V, {msg, _}, Idx) when is_tuple(V) ->
    sort_deterministic(V, Idx);
sort_map_value(V, _, _) ->
    V.

%% Stable sort: the index tag keeps duplicate keys in input order, so the
%% winner on decode (last value wins) is deterministic too.
sort_entries(Entries, KType, VType, Idx) ->
    Signed = is_signed_key(KType),
    Tagged = [{canon_key(K, KType), I, {K, sort_map_value(V, VType, Idx)}} || {I, {K, V}} <- lists:zip(lists:seq(1, length(Entries)), Entries)],
    [E || {_, _, E} <- lists:sort(fun(A, B) -> entry_lt(A, B, Signed) end, Tagged)].

entry_lt({KA, IA, _}, {KB, IB, _}, _Signed) when KA =:= KB ->
    IA < IB;
entry_lt({KA, _, _}, {KB, _, _}, _Signed) when is_binary(KA), is_binary(KB) ->
    KA < KB;
entry_lt({KA, _, _}, {KB, _, _}, true) when is_integer(KA), is_integer(KB) ->
    KA < KB;
entry_lt({KA, _, _}, {KB, _, _}, false) when is_integer(KA), is_integer(KB), KA >= 0, KB >= 0 ->
    KA < KB;
entry_lt({KA, _, _}, {KB, _, _}, _) ->
    KA < KB.

%% Canonical key for comparison, mirroring the core's map_key_cmp: strings
%% byte-wise, signed integers signed, everything else unsigned.
canon_key(K, string) ->
    iolist_to_binary(K);
canon_key(true, bool) ->
    1;
canon_key(false, bool) ->
    0;
canon_key(K, T) when is_integer(K) ->
    case is_signed_key(T) of
        true -> K;
        false -> K band key_mask(T)
    end;
canon_key(K, _) ->
    K.

is_signed_key(int32) -> true;
is_signed_key(int64) -> true;
is_signed_key(sint32) -> true;
is_signed_key(sint64) -> true;
is_signed_key(sfixed32) -> true;
is_signed_key(sfixed64) -> true;
is_signed_key(_) -> false.

key_mask(uint32) -> 16#FFFFFFFF;
key_mask(fixed32) -> 16#FFFFFFFF;
key_mask(_) -> 16#FFFFFFFFFFFFFFFF.
