-module(ep_issue_29_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

% https://github.com/jinganix/pbwire/issues/29
issue_29_test() ->
    Defs = [
        {{msg, a_message}, [
            {field, non_trivial_map, 1, 2, {map, string, {msg, non_trivial_item}}, repeated, []}
        ]},
        {{msg, non_trivial_item}, [
            {field, item, 1, 2, int64, optional, []}
        ]}
    ],
    Msg = {a_message, [{"a", {non_trivial_item, 1}}]},
    Bin = <<10, 7, 10, 1, 97, 18, 2, 8, 1>>,
    ok = pbwire:set_opts([{string_as_list, true}, {with_utf8, true}]),
    ok = pbwire:load_cache(Defs),
    Bin = gpb:encode_msg(Msg, Defs),
    Bin = pbwire:encode_msg(Msg, Defs),
    Msg = pbwire:decode_msg(Bin, a_message, Defs).
