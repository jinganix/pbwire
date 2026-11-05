%% Nested maps (a map value that is itself a map) round trip through the
%% NIF: the schema synthesizes a repeated entry field for the inner level,
%% so inner entries merge by key and keep wire order like any repeated
%% field. gpb cannot express this shape, so the reference bytes are pinned
%% by hand (field 1, int32 keys/values throughout).
-module(ep_nested_map_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

%% {doc, [{1, [{10, 100}, {20, 200}]}, {2, []}]}:
%%   0a 0f 08 01 12 04 08 0a 10 64 12 05 08 14 10 c8 01 0a 02 08 02
-define(REF, <<10, 15, 8, 1, 18, 4, 8, 10, 16, 100,
              18, 5, 8, 20, 16, 200, 1, 10, 2, 8, 2>>).

nested_map_defs() ->
    Type = {map, int32, {map, int32, int32}},
    [{{msg, nested_doc},
      [#?gpb_field{name = nested, fnum = 1, rnum = 2, type = Type,
                   occurrence = repeated, opts = []}]}].

nested_map_roundtrip_test() ->
    ok = pbwire:load_cache(nested_map_defs()),
    Msg = {nested_doc, [{1, [{10, 100}, {20, 200}]}, {2, []}]},
    ?assertEqual(?REF, pbwire:encode(Msg)),
    ?assertEqual(Msg, pbwire:decode(?REF, nested_doc)),
    %% Inner entries merge by key, last wins, like top-level maps.
    Merged = {nested_doc, [{1, [{10, 100}, {10, 7}]}]},
    MergedBin = pbwire:encode(Merged),
    ?assertEqual({nested_doc, [{1, [{10, 7}]}]},
                 pbwire:decode(MergedBin, nested_doc)),
    ok.
