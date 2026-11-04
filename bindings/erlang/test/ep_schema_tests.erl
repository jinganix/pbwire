%% load_cache/1: the schema comes from a gpb module's introspection list, so it
%% has to accept that list as gpb emits it (every field kind, oneofs, map
%% fields, aliases, and the gpb 5 defs-format-3 shapes) and resolve the message
%% and enum references between the definitions.
-module(ep_schema_tests).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

-record(m1, {a}).

%% Every field kind and occurrence in one schema, with options that are
%% irrelevant for the wire type (`packed` on non-packable types) and a default
%% that is not a valid value for the type: load_cache/1 must not care.
load_cache_accepts_every_field_kind_test() ->
    ok = pbwire:load_cache([
        {{msg, m1}, [
            {field, int32, 1, 1, int32, optional, [packed, {default, ok}]},
            {field, int64, 2, 2, int64, optional, [packed, {default, ok}]},
            {field, uint32, 3, 3, uint32, optional, [packed, {default, ok}]},
            {field, uint64, 4, 4, uint64, optional, [packed, {default, ok}]},
            {field, sint32, 5, 5, sint32, optional, [packed, {default, ok}]},
            {field, sint64, 6, 6, sint64, optional, [packed, {default, ok}]},
            {field, fixed32, 7, 7, fixed32, optional, [packed, {default, ok}]},
            {field, fixed64, 8, 8, fixed64, optional, [packed, {default, ok}]},
            {field, sfixed32, 9, 9, sfixed32, optional, [packed, {default, ok}]},
            {field, sfixed64, 10, 10, sfixed64, optional, [packed, {default, ok}]},
            {field, bool, 11, 11, bool, optional, [packed, {default, ok}]},
            {field, float, 12, 12, float, optional, [packed, {default, ok}]},
            {field, double, 13, 13, double, optional, [packed, {default, ok}]},
            {field, string, 14, 14, string, optional, [packed, {default, ok}]},
            {field, bytes, 15, 15, bytes, optional, [packed, {default, ok}]},
            {field, enum, 16, 16, {enum, e}, optional, [packed, {default, ok}]},
            {field, msg, 17, 17, {msg, m2}, optional, [packed, {default, ok}]},
            {field, map, 18, 18, {map, string, fixed32}, optional, [packed, {default, ok}]},
            {field, required, 19, 19, fixed32, required, [packed, {default, ok}]},
            {field, optional, 20, 20, fixed32, optional, [packed, {default, ok}]},
            {field, repeated, 21, 21, fixed32, repeated, [packed, {default, ok}]},
            {gpb_oneof, oneof, 22, [
                {field, int32, 22, 22, int32, optional, [packed, {default, ok}]},
                {field, int64, 23, 22, int64, optional, [packed, {default, ok}]}
            ]}
        ]},
        {{msg, m2}, [
            {field, int32, 0, 1, int32, optional, [packed, {default, ok}]}
        ]},
        {{msg, m3}, [
            {field, int32, 0, 1, int32, optional, [packed, {default, ok}]},
            {field, int64, 1, 2, int64, optional, [packed, {default, ok}]}
        ]},
        {{enum, e}, [
            {v1, 100},
            {v2, -2},
            {v3, -2},
            {option, allow_alias, true}
        ]}
    ]).

%% A oneof whose members are scalars, plus a plain field after it.
load_cache_oneof_test() ->
    ok = pbwire:load_cache([{{msg, 'Person'}, [
        #gpb_oneof{name = message_body, rnum = 2, fields = [
            #field{name = file_children, fnum = 3, rnum = 2, type = int32, occurrence = optional, opts = []},
            #field{name = xattr, fnum = 4, rnum = 2, type = bytes, occurrence = optional, opts = []}]},
        #field{name = proxy_session_id, fnum = 21, rnum = 3, type = bytes, occurrence = optional, opts = []}
    ]}]).

%% A oneof nested under a message, which is itself inside a oneof: the linker
%% has to resolve the whole tree before the first encode.
nested_oneof_roundtrip_test() ->
    Defs = [
        {{msg, 'ChildLink'}, [
            #field{name = name, fnum = 2, rnum = 2, type = bytes, occurrence = required, opts = []}
        ]},
        {{msg, 'FileChildren'}, [
            #field{name = child_links, fnum = 1, rnum = 2, type = {msg, 'ChildLink'}, occurrence = repeated, opts = []}
        ]},
        {{msg, 'FuseResponse'}, [
            #gpb_oneof{name = fuse_response, rnum = 2, fields = [
                #field{name = file_children, fnum = 3, rnum = 2, type = {msg, 'FileChildren'}, occurrence = optional, opts = []},
                #field{name = xattr, fnum = 13, rnum = 2, type = bytes, occurrence = optional, opts = []}
            ]}
        ]},
        {{msg, 'ServerMessage'}, [
            #gpb_oneof{name = message_body, rnum = 2, fields = [
                #field{name = fuse_response, fnum = 15, rnum = 2, type = {msg, 'FuseResponse'}, occurrence = optional, opts = []}
            ]}
        ]}
    ],
    ok = pbwire:load_cache(Defs),
    Msg = {'ServerMessage',
        {fuse_response,
            {'FuseResponse',
                {file_children,
                    {'FileChildren', [{'ChildLink', <<"1">>}]}
                }
            }
        }
    },
    Bin = pbwire:encode(Msg),
    Msg = pbwire:decode(Bin, 'ServerMessage').

%% gpb 5 (defs format 3) emits enum values as {Sym, Value, Options} and keeps
%% the proto3 markers only in get_proto_defs/0, next to metadata entries such
%% as {proto_defs_version, 4} and {file, _}. load_cache/1 accepts that list
%% as-is; gpb has already verified the proto, so repeated values are aliases.
defs_format_3_enum_test() ->
    Defs = [{proto_defs_version, 4},
        {file, {"m", "m.proto"}},
        {syntax, "proto3"},
        {package, pkg},
        {{enum, e}, [{v1, 100, []},
            {v2, 100, []},
            {v3, 150, []}]},
        {{msg, m1}, [#?gpb_field{name = a, fnum = 1, rnum = #m1.a,
            type = {enum, e}, occurrence = defaulty,
            opts = []}]}],
    ok = pbwire:load_cache(Defs),
    #m1{a = v1} = pbwire:decode(<<8, 100>>, m1),
    #m1{a = v3} = pbwire:decode(<<8, 150, 1>>, m1),
    <<8, 100>> = pbwire:encode(#m1{a = v1}),
    <<8, 100>> = pbwire:encode(#m1{a = v2}).
