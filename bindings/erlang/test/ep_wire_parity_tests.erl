%% Wire-level behaviour that must match gpb, exercising the real NIF.
-module(ep_wire_parity_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

field(Name, Fnum, Rnum, Type, Occ) ->
    #field{name = Name, fnum = Fnum, rnum = Rnum, type = Type, occurrence = Occ, opts = []}.

varint(N) when N < 128 -> <<N>>;
varint(N) -> <<(16#80 bor (N band 16#7F)), (varint(N bsr 7))/binary>>.

%% Any non-zero varint decodes to true and the whole varint is consumed, so the
%% next tag is not misread (gpb decodes `Value =/= 0`).
bool_varint_parity_test() ->
    Defs = [{{msg, b}, [field(a, 1, 2, bool, optional)]}],
    ok = pbwire:load_cache(Defs),
    Cases = [{<<8, 16#80, 1>>, true}, {<<8, 2>>, true}, {<<8, 0>>, false}],
    lists:foreach(
        fun({Wire, Expected}) ->
            ?assertEqual({b, Expected}, pbwire:decode(Wire, b)),
            ?assertEqual({b, Expected}, gpb:decode_msg(Wire, b, Defs))
        end, Cases),
    ?assertEqual(gpb:encode_msg({b, true}, Defs), pbwire:encode({b, true})),
    ?assertEqual(gpb:encode_msg({b, false}, Defs), pbwire:encode({b, false})).

%% Zigzag values are masked to the declared width before decoding, like gpb.
sint_extremes_and_oversized_parity_test() ->
    Defs = [{{msg, s}, [field(a, 1, 2, sint32, optional), field(c, 2, 3, sint64, optional)]}],
    ok = pbwire:load_cache(Defs),
    lists:foreach(
        fun(V) ->
            Bin = gpb:encode_msg({s, V, 0}, Defs),
            ?assertEqual(Bin, pbwire:encode({s, V, 0})),
            ?assertEqual({s, V, 0}, pbwire:decode(Bin, s))
        end, [-2147483648, -1, 0, 1, 2147483647]),
    %% varint 2^32+1: gpb masks to 32 bits first, giving -1 rather than INT32_MIN
    Oversized = <<8, 16#81, 16#80, 16#80, 16#80, 16#10>>,
    ?assertEqual(gpb:decode_msg(Oversized, s, Defs), pbwire:decode(Oversized, s)).

%% proto3 omits the enum type default (value 0); the other values are sent even
%% when their name sorts first.
proto3_enum_default_parity_test() ->
    Defs = [{syntax, "proto3"},
            {proto3_msgs, [e]},
            {{enum, en}, [{zzz_zero, 0}, {aaa_one, 1}]},
            {{msg, e}, [field(v, 1, 2, {enum, en}, defaulty)]}],
    ok = pbwire:load_cache(Defs),
    ?assertEqual(gpb:encode_msg({e, aaa_one}, Defs), pbwire:encode({e, aaa_one})),
    ?assertEqual(<<>>, pbwire:encode({e, zzz_zero})),
    ?assertEqual({e, zzz_zero}, pbwire:decode(<<>>, e)).

%% Deeply nested messages grow the decoder's spot stack; the result must match
%% gpb instead of crashing or truncating.
deep_recursive_message_parity_test() ->
    Defs = [{{msg, r}, [field(a, 1, 2, {msg, r}, optional)]}],
    ok = pbwire:load_cache(Defs),
    Wire = lists:foldl(fun(_, Acc) -> <<10, (varint(byte_size(Acc)))/binary, Acc/binary>> end,
                       <<>>, lists:seq(1, 200)),
    ?assertEqual(gpb:decode_msg(Wire, r, Defs), pbwire:decode(Wire, r)).

%% A charlist longer than the encoder's initial buffer must grow it safely.
long_utf8_charlist_roundtrip_test() ->
    ok = pbwire:set_opts([{with_utf8, true}, {string_as_list, true}]),
    ok = pbwire:load_cache([{{msg, u}, [field(s, 1, 2, string, optional)]}]),
    Long = lists:duplicate(20000, 16#800),
    Bin = pbwire:encode({u, Long}),
    ?assertEqual({u, Long}, pbwire:decode(Bin, u)).

%% A schema may refer to its own message. load_cache/1 has to link that
%% reference, and a tree of nested children must round-trip byte for byte.
self_recursive_message_roundtrip_test() ->
    Defs = [{{msg, p_v}, [
        field(int_v, 1, 2, int64, optional),
        field(str_v, 2, 3, string, optional),
        field(float_v, 3, 4, float, optional),
        field(list, 4, 5, {msg, p_v}, repeated)
    ]}],
    ok = pbwire:set_opts([{string_as_list, true}, {with_utf8, true}]),
    ok = pbwire:load_cache(Defs),
    Msg = {p_v, 1001, "root", 1.5, [
        {p_v, 1002, "child_1", 2.5, []},
        {p_v, 1003, "child_2", 3.5, [{p_v, 1004, "leaf", 4.5, []}]}
    ]},
    Bin = gpb:encode_msg(Msg, Defs),
    ?assertEqual(Bin, pbwire:encode(Msg)),
    ?assertEqual(Msg, pbwire:decode(Bin, p_v)),
    ?assertEqual(Msg, gpb:decode_msg(Bin, p_v, Defs)).

%% A captured real-world message: repeated nested messages, UTF-8 strings and
%% field numbers above 15 (two-byte tags). Both codecs must agree on the bytes
%% in both directions.
real_world_message_parity_test() ->
    Defs = real_world_defs(),
    Wire = real_world_wire(),
    ok = pbwire:set_opts([{with_utf8, true}]),
    ok = pbwire:load_cache(Defs),
    GpbMsg = gpb:decode_msg(Wire, auth2s, Defs),
    EpbMsg = pbwire:decode(Wire, auth2s),
    GpbBin = gpb:encode_msg(GpbMsg, Defs),
    EpbBin = pbwire:encode(EpbMsg),
    ?assertEqual(GpbBin, EpbBin),
    ?assertEqual(GpbMsg, gpb:decode_msg(EpbBin, auth2s, Defs)),
    ?assertEqual(EpbMsg, pbwire:decode(GpbBin, auth2s)).

real_world_defs() ->
    [
        {{msg, p_ks}, [
            #field{name = id, fnum = 1, rnum = 2, type = int64, occurrence = optional, opts = []},
            #field{name = str, fnum = 2, rnum = 3, type = string, occurrence = optional, opts = []}
        ]},
        {{msg, auth2s}, [
            #field{name = uid, fnum = 1, rnum = 2, type = string, occurrence = optional, opts = []},
            #field{name = uname, fnum = 2, rnum = 3, type = string, occurrence = optional, opts = []},
            #field{name = s0, fnum = 3, rnum = 4, type = string, occurrence = optional, opts = []},
            #field{name = time, fnum = 4, rnum = 5, type = int64, occurrence = optional, opts = []},
            #field{name = id1, fnum = 5, rnum = 6, type = int64, occurrence = optional, opts = []},
            #field{name = id2, fnum = 6, rnum = 7, type = int64, occurrence = optional, opts = []},
            #field{name = id3, fnum = 7, rnum = 8, type = int64, occurrence = optional, opts = []},
            #field{name = id4, fnum = 8, rnum = 9, type = int64, occurrence = optional, opts = []},
            #field{name = id5, fnum = 9, rnum = 10, type = int64, occurrence = optional, opts = []},
            #field{name = s1, fnum = 10, rnum = 11, type = string, occurrence = optional, opts = []},
            #field{name = s2, fnum = 11, rnum = 12, type = string, occurrence = optional, opts = []},
            #field{name = id6, fnum = 12, rnum = 13, type = int64, occurrence = optional, opts = []},
            #field{name = s3, fnum = 13, rnum = 14, type = string, occurrence = repeated, opts = []},
            #field{name = ks1, fnum = 14, rnum = 15, type = {msg, p_ks}, occurrence = repeated, opts = []},
            #field{name = ks2, fnum = 15, rnum = 16, type = {msg, p_ks}, occurrence = repeated, opts = []},
            #field{name = id7, fnum = 16, rnum = 17, type = int64, occurrence = optional, opts = []}
        ]}
    ].

real_world_wire() ->
    <<48, 1, 82, 10, 55, 48, 49, 95, 49, 57, 48, 51, 95, 48, 32, 201, 220, 202,
        190, 6, 90, 16, 49, 46, 48, 46, 48, 46, 57, 54, 54, 46, 49, 51, 49, 50,
        56, 48, 114, 2, 8, 1, 114, 2, 8, 2, 114, 7, 8, 3, 18, 3, 110, 105, 108,
        114, 23, 8, 4, 18, 19, 55, 48, 49, 45, 49, 57, 48, 51, 45, 49, 55, 52,
        49, 55, 54, 50, 49, 50, 49, 114, 18, 8, 21, 18, 14, 87, 105, 110, 100,
        111, 119, 115, 32, 49, 48, 32, 120, 54, 52, 114, 2, 8, 6, 114, 12, 8,
        10, 18, 8, 57, 54, 50, 42, 49, 54, 56, 48, 114, 2, 8, 13, 114, 5, 8, 14,
        18, 1, 48, 114, 2, 8, 15, 114, 10, 8, 16, 18, 6, 229, 144, 155, 228,
        184, 187, 56, 239, 14, 106, 24, 105, 87, 88, 116, 78, 80, 51, 104, 54,
        73, 51, 35, 80, 67, 76, 106, 116, 69, 75, 90, 67, 98, 111, 88, 106, 17,
        95, 49, 55, 48, 52, 56, 56, 48, 51, 48, 57, 54, 54, 53, 49, 55, 48, 64,
        239, 14, 10, 9, 53, 56, 50, 57, 48, 51, 50, 52, 57, 40, 189, 5, 122, 5,
        8, 7, 18, 1, 48, 122, 8, 8, 8, 18, 4, 49, 48, 48, 48, 122, 5, 8, 4, 18,
        1, 48, 18, 9, 53, 56, 50, 57, 48, 51, 50, 52, 57>>.
