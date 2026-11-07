-module(ep_issue_31_tests).

-compile(export_all).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

% https://github.com/jinganix/pbwire/issues/31
% A 100k-value enum takes seconds to link; the default 5 s EUnit timeout
% flakes on slower runners.
issue_31_test_() ->
    {timeout, 60, fun issue_31_test/0}.

issue_31_test() ->
    Defs = [
        {{enum, very_long}, defs_enum_gen(100000, [])}
    ],

    pbwire:purge_cache(),
    pbwire:load_cache(Defs).

defs_enum_gen(0, OptList) -> OptList;
defs_enum_gen(N, CurOptList) when N > 0 ->
    defs_enum_gen(N-1, [{list_to_atom("opt" ++ integer_to_list(N)), N} | CurOptList]).
