%% Core coverage over the shared NIF: length-delimited framing, deterministic
%% map ordering and the max_bytes/max_depth input guards (the pbw_limits
%% twins). JSON and text stay non-goals here: they would need a gpb-to-core
%% schema mirror (a second loader) or an Erlang JSON mapping (a second
%% codec), and the binding keeps gpb as its only schema source.
-module(ep_core_coverage_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

map_defs() ->
    [{{msg, m}, [
        #field{name = s, fnum = 1, rnum = 2, type = {map, string, int32},
               occurrence = repeated, opts = []},
        #field{name = i, fnum = 2, rnum = 3, type = {map, int32, int32},
               occurrence = repeated, opts = []},
        #field{name = u, fnum = 3, rnum = 4, type = {map, fixed32, int32},
               occurrence = repeated, opts = []},
        #field{name = r, fnum = 4, rnum = 5, type = int32,
               occurrence = repeated, opts = []}
    ]}].

nested_defs() ->
    [{{msg, inner}, [
        #field{name = tags, fnum = 1, rnum = 2, type = {map, string, int32},
               occurrence = repeated, opts = []}
    ]},
     {{msg, outer}, [
        #field{name = subs, fnum = 1, rnum = 2, type = {map, string, {msg, inner}},
               occurrence = repeated, opts = []},
        #field{name = nums, fnum = 2, rnum = 3, type = int32,
               occurrence = repeated, opts = []}
    ]}].

chain_defs() ->
    [{{msg, chain}, [
        #field{name = sub, fnum = 1, rnum = 2, type = {msg, chain},
               occurrence = optional, opts = []},
        #field{name = v, fnum = 2, rnum = 3, type = int32,
               occurrence = optional, opts = []}
    ]}].

deep_chain(0) -> {chain, undefined, 0};
deep_chain(N) -> {chain, deep_chain(N - 1), N}.

%% -- delimited framing -------------------------------------------------

delimited_roundtrip_test() ->
    Defs = ep_test_helpers:person_defs(),
    Msg = {'Person', "abc", 1, undefined},
    Bin = ep_test_helpers:encode_msg(Msg, Defs),
    Frame = pbwire:encode_delimited(Msg),
    %% varint length prefix followed by the plain encoding.
    ?assertEqual(<<7, Bin/binary>>, Frame),
    ?assertEqual({Msg, <<>>}, pbwire:decode_delimited(Frame, 'Person')).

delimited_concatenated_test() ->
    Defs = ep_test_helpers:person_defs(),
    M1 = {'Person', "a", 1, undefined},
    M2 = {'Person', "b", 2, "x@y"},
    _ = ep_test_helpers:load_cache(Defs),
    Stream = <<(pbwire:encode_delimited(M1))/binary,
               (pbwire:encode_delimited(M2))/binary>>,
    {Back1, Rest} = pbwire:decode_delimited(Stream, 'Person'),
    ?assertEqual(M1, Back1),
    ?assertEqual({M2, <<>>}, pbwire:decode_delimited(Rest, 'Person')).

delimited_truncated_test() ->
    Defs = ep_test_helpers:person_defs(),
    _ = ep_test_helpers:load_cache(Defs),
    %% Prefix promises 5 bytes, only 2 follow.
    ?assertError(_, pbwire:decode_delimited(<<5, 1, 2>>, 'Person')),
    %% Six-byte varint: not a varint32.
    ?assertError(_, pbwire:decode_delimited(<<255, 255, 255, 255, 255, 1>>, 'Person')),
    %% Empty input has no prefix at all.
    ?assertError(_, pbwire:decode_delimited(<<>>, 'Person')),
    ?assertError(badarg, pbwire:decode_delimited(not_a_binary, 'Person')),
    ?assertError(badarg, pbwire:decode_delimited(<<1>>, 42)).

%% -- deterministic encoding --------------------------------------------

deterministic_map_order_test() ->
    Defs = map_defs(),
    M1 = {m, [{"b", 2}, {"a", 1}], [{3, 30}, {-5, 50}, {0, 0}], [], []},
    M2 = {m, [{"a", 1}, {"b", 2}], [{0, 0}, {3, 30}, {-5, 50}], [], []},
    Sorted = {m, [{"a", 1}, {"b", 2}], [{-5, 50}, {0, 0}, {3, 30}], [], []},
    %% Insertion order does not matter ...
    ?assertEqual(pbwire:encode_deterministic(M1, Defs),
                 pbwire:encode_deterministic(M2, Defs)),
    %% ... and the bytes equal the plain encoding of the sorted shape ...
    ?assertEqual(ep_test_helpers:encode_msg(Sorted, Defs),
                 pbwire:encode_deterministic(M1, Defs)),
    %% ... which decodes back in wire order.
    ?assertEqual(Sorted,
                 ep_test_helpers:decode_msg(pbwire:encode_deterministic(M1, Defs), m, Defs)).

deterministic_signed_unsigned_test() ->
    Defs = map_defs(),
    %% int32 keys sort signed: -1 before 3.
    I = {m, [], [{3, 1}, {-1, 2}], [], []},
    ?assertEqual({m, [], [{-1, 2}, {3, 1}], [], []},
                 ep_test_helpers:decode_msg(pbwire:encode_deterministic(I, Defs), m, Defs)),
    %% fixed32 keys sort unsigned: -1 rides the wire as 0xFFFFFFFF, after 3
    %% (and decodes back as the unsigned value the NIF materializes).
    U = {m, [], [], [{3, 1}, {-1, 2}], []},
    ?assertEqual({m, [], [], [{3, 1}, {16#FFFFFFFF, 2}], []},
                 ep_test_helpers:decode_msg(pbwire:encode_deterministic(U, Defs), m, Defs)).

deterministic_nested_test() ->
    Defs = nested_defs(),
    A = {outer, [{"z", {inner, [{"b", 1}, {"a", 2}]}}, {"a", {inner, []}}], [3, 1, 2]},
    B = {outer, [{"a", {inner, []}}, {"z", {inner, [{"a", 2}, {"b", 1}]}}], [3, 1, 2]},
    ?assertEqual(pbwire:encode_deterministic(A, Defs),
                 pbwire:encode_deterministic(B, Defs)),
    %% Repeated fields keep their order: B is already sorted, so its plain
    %% encoding (nums [3, 1, 2] untouched) matches.
    ?assertEqual(ep_test_helpers:encode_msg(B, Defs),
                 pbwire:encode_deterministic(A, Defs)).

deterministic_keeps_repeated_order_test() ->
    Defs = map_defs(),
    Msg = {m, [], [], [], [3, 1, 2]},
    ?assertEqual(ep_test_helpers:encode_msg(Msg, Defs),
                 pbwire:encode_deterministic(Msg, Defs)).

%% -- limits ------------------------------------------------------------

limits_max_bytes_test() ->
    Defs = ep_test_helpers:person_defs(),
    Msg = {'Person', "abc def", 345, "a@example.com"},
    Bin = ep_test_helpers:encode_msg(Msg, Defs),
    Size = byte_size(Bin),
    Frame = pbwire:encode_delimited(Msg),
    ?assertMatch({'Person', _, _, _}, pbwire:decode(Bin, 'Person')),
    try
        ok = pbwire:set_opts([{max_bytes, Size - 1}]),
        ?assertError(too_large, pbwire:decode(Bin, 'Person')),
        %% Delimited frames go through the same guard (the frame is Size).
        ?assertError(too_large, pbwire:decode_delimited(Frame, 'Person')),
        ok = pbwire:set_opts([{max_bytes, Size}]),
        ?assertMatch({'Person', _, _, _}, pbwire:decode(Bin, 'Person')),
        ?assertMatch({{'Person', _, _, _}, <<>>}, pbwire:decode_delimited(Frame, 'Person'))
    after
        pbwire:set_opts([{max_bytes, 0}])
    end,
    ?assertMatch({'Person', _, _, _}, pbwire:decode(Bin, 'Person')).

limits_max_depth_test() ->
    Defs = chain_defs(),
    Deep = deep_chain(10),
    Bin = ep_test_helpers:encode_msg(Deep, Defs),
    ?assertEqual(Deep, pbwire:decode(Bin, chain)),
    try
        ok = pbwire:set_opts([{max_depth, 3}]),
        ?assertError(too_deep, pbwire:decode(Bin, chain)),
        ok = pbwire:set_opts([{max_depth, 0}]),
        ?assertEqual(Deep, pbwire:decode(Bin, chain))
    after
        pbwire:set_opts([{max_depth, 0}])
    end.

limits_badarg_test() ->
    ?assertError(badarg, pbwire:set_opts([{max_bytes, -1}])),
    ?assertError(badarg, pbwire:set_opts([{max_bytes, x}])),
    ?assertError(badarg, pbwire:set_opts([{max_depth, -1}])),
    ?assertError(badarg, pbwire:set_opts([{max_depth, 1.5}])).
