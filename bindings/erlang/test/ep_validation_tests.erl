%% Boundary validation: everything crossing in from Erlang is untrusted, so a
%% malformed def list, field tuple, option list or message record must raise
%% instead of corrupting the NIF. Each case pins one shape the C parser has to
%% reject (or one improper list the encoders have to notice).
-module(ep_validation_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

%% -------------------------------------------------------------
%% load_cache/1: the defs list itself

load_cache_non_list_should_fail_test() ->
    ?assertError(not_a_list, pbwire:load_cache(not_a_list)).

%% An improper defs list silently drops everything after the tail, so callers
%% would end up with a partial schema. Reject it.
load_cache_improper_defs_list_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #field{name = a, fnum = 1, rnum = 2, type = int32, occurrence = required, opts = []}
    ]},
        {{msg, m2}, [
            #field{name = b, fnum = 1, rnum = 2, type = int32, occurrence = required, opts = []}
        ]}
        | not_a_proper_list],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_field_opts_improper_list_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #field{name = a, fnum = 1, rnum = 2, type = int32, occurrence = repeated,
            opts = [packed | not_a_proper_list]}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_msg_fields_improper_list_should_fail_test() ->
    Defs = [
        {{enum, e}, [{v1, 0}]},
        {{msg, m1}, [
            #field{name = a, fnum = 1, rnum = 2, type = {enum, e}, occurrence = required, opts = []},
            #field{name = b, fnum = 2, rnum = 3, type = int32, occurrence = optional, opts = []}
            | not_a_proper_list
        ]}
    ],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_proto3_msgs_improper_list_should_fail_test() ->
    Defs = [
        {syntax, "proto2"},
        {proto3_msgs, [m1, m2 | not_a_proper_list]},
        {{msg, m1}, [
            #field{name = a, fnum = 1, rnum = 2, type = uint32, occurrence = defaulty, opts = []}
        ]},
        {{msg, m2}, [
            #field{name = b, fnum = 1, rnum = 2, type = uint32, occurrence = defaulty, opts = []}
        ]}
    ],
    ?assertError(_, pbwire:load_cache(Defs)).

%% A map key has to be a scalar protobuf allows. A map value may itself be
%% a map (wire shape is entries in entries); see ep_nested_map_tests.
load_cache_unsupported_map_key_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #field{name = m, fnum = 1, rnum = 2, type = {map, double, int32},
            occurrence = repeated, opts = []}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_nested_map_value_loads_test() ->
    Defs = [{{msg, m1}, [
        #field{name = m, fnum = 1, rnum = 2, type = {map, string, {map, int32, int32}},
            occurrence = repeated, opts = []}
    ]}],
    ?assertEqual(ok, pbwire:load_cache(Defs)).

%% -------------------------------------------------------------
%% load_cache/1: enums and oneofs

load_cache_enum_values_improper_list_should_fail_test() ->
    Defs = [{{enum, e}, [{v1, 0}, {v2, 1} | not_a_proper_list]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_enum_invalid_tuple_after_allow_alias_should_fail_test() ->
    Defs = [{{enum, e}, [
        {option, allow_alias, true},
        {v1, 0},
        {unexpected, tuple, shape}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_enum_duplicate_name_should_fail_test() ->
    Defs = [{{enum, e}, [{v1, 0}, {v1, 1}]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_enum_duplicate_value_without_alias_should_fail_test() ->
    Defs = [{{enum, e}, [{v1, 0}, {v2, 0}]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_oneof_fields_improper_list_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #gpb_oneof{name = a, rnum = 2, fields = [
            #field{name = a1, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []}
            | not_a_proper_list
        ]}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_oneof_wrong_arity_should_fail_test() ->
    Defs = [{{msg, m1}, [{gpb_oneof, a, 2}]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_oneof_non_atom_name_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #gpb_oneof{name = 123, rnum = 2, fields = [
            #field{name = a1, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []}
        ]}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_oneof_extra_elements_should_fail_test() ->
    Defs = [{{msg, m1}, [
        {gpb_oneof, a, 2, [
            #field{name = a1, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []}
        ], [], unexpected_extra}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

load_cache_oneof_invalid_opts_should_fail_test() ->
    Defs = [{{msg, m1}, [
        {gpb_oneof, a, 2, [
            #field{name = a1, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []}
        ], invalid_opts_type}
    ]}],
    ?assertError(_, pbwire:load_cache(Defs)).

%% -------------------------------------------------------------
%% encode/1: the message record itself

%% gpb raises on improper repeated lists; so does the NIF.
repeated_int32_improper_list_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #field{name = a, fnum = 1, rnum = 2, type = int32, occurrence = repeated, opts = []}
    ]}],
    ok = pbwire:load_cache(Defs),
    Msg = {m1, [1, 2 | not_proper]},
    ?assertError(_, pbwire:encode(Msg)).

unknown_fields_not_a_list_should_fail_test() ->
    Defs = [{{msg, m1}, [
        #field{name = a, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []},
        #field{name = '$unknown', fnum = undefined, rnum = 3, type = unknown,
            occurrence = repeated, opts = []}
    ]}],
    ok = pbwire:load_cache(Defs),
    ?assertError(_, pbwire:encode({m1, 1, not_a_list})),
    ?assertError(_, pbwire:encode({m1, 1, [{varint, 2, 3} | improper_tail]})).

repeated_msg_element_extra_field_should_fail_test() ->
    Defs = repeated_msg_defs(),
    ok = pbwire:load_cache(Defs),
    ?assertError(_, pbwire:encode({parent, [{child, 1, ignored_extra_field}]})).

repeated_msg_element_missing_field_should_fail_test() ->
    Defs = repeated_msg_defs(),
    ok = pbwire:load_cache(Defs),
    ?assertError(_, pbwire:encode({parent, [{child}]})).

encode_nested_msg_with_wrong_name_should_fail_test() ->
    Defs = repeated_msg_defs(),
    ok = pbwire:load_cache(Defs),
    ?assertError(_, pbwire:encode({parent, [{wrong_child, 1}]})).

repeated_msg_defs() ->
    [
        {{msg, child}, [
            #field{name = v, fnum = 1, rnum = 2, type = int32, occurrence = optional, opts = []}
        ]},
        {{msg, parent}, [
            #field{name = children, fnum = 1, rnum = 2, type = {msg, child},
                occurrence = repeated, opts = []}
        ]}
    ].

%% -------------------------------------------------------------
%% set_opts/1

set_opts_improper_list_should_fail_test() ->
    ?assertError(badarg, pbwire:set_opts([{with_utf8, true} | not_proper])).

set_opts_unknown_option_should_fail_test() ->
    ?assertError(badarg, pbwire:set_opts([{unknown_opt, true}])).

set_opts_non_boolean_value_should_fail_test() ->
    ?assertError(badarg, pbwire:set_opts([{with_utf8, not_bool}])).
