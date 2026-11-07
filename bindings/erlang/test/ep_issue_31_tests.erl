-module(ep_issue_31_tests).

-compile(export_all).

-include_lib("eunit/include/eunit.hrl").
-include_lib("gpb/include/gpb.hrl").

% https://github.com/jinganix/pbwire/issues/31
% A 100k-value enum takes seconds to link; the default 5 s EUnit timeout
% flakes on slower runners.
issue_31_test_() ->
    {timeout, 60, fun issue_31/0}.

%% Not named issue_31_test/0: EUnit auto-discovers *_test/0 functions with the
%% default 5 s timeout, so the same workload ran twice and the second run timed
%% out whenever the machine took longer than 5 s to link the enum.
issue_31() ->
    Defs = [
        {{enum, very_long}, defs_enum_gen(100000, [])}
    ],

    pbwire:purge_cache(),
    pbwire:load_cache(Defs).

defs_enum_gen(0, OptList) -> OptList;
defs_enum_gen(N, CurOptList) when N > 0 ->
    defs_enum_gen(N-1, [{list_to_atom("opt" ++ integer_to_list(N)), N} | CurOptList]).
