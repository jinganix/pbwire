%% Shared Proper generators for the encode and decode property suites: values
%% in the range gpb accepts for every wire type, plus UTF-8 charlists.
-module(ep_proper_gen).

-ifdef(PROPER).
-include_lib("proper/include/proper.hrl").

-export([
    ascii_string/0,
    sint32/0,
    sint64/0,
    uint32/0,
    uint64/0,
    utf8string/0,
    value/0
]).

utf8char() ->
    union([
        integer(0, 36095),
        integer(57344, 65533),
        integer(65536, 1114111)
    ]).

utf8string() -> list(utf8char()).

ascii_string() -> list(choose(0, 127)).

uint32() -> choose(0, 4294967295).

sint32() -> choose(-2147483648, 2147483647).

uint64() -> choose(0, 18446744073709551615).

sint64() ->
    choose(-9223372036854775808, 9223372036854775807).

value() ->
    oneof([{real(), double}, {real(), float}, {nan, float},
        {infinity, float}, {'-infinity', float}, {nan, double},
        {infinity, double}, {'-infinity', double},
        {uint32(), uint32}, {uint64(), uint64},
        {sint32(), sint32}, {sint64(), sint64},
        {uint32(), fixed32}, {uint64(), fixed64},
        {sint32(), sfixed32}, {sint64(), sfixed64},
        {sint32(), int32}, {sint64(), int64}, {bool(), bool},
        {sint32(), enum}, {utf8string(), string},
        {binary(), bytes}]).

-endif.
