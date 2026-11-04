%% load_cache/1 replaces the schema for the whole node, and encode/decode run
%% under a read lock: many processes may encode and decode while another one
%% reloads the same schema. These stress tests mix both until every worker has
%% seen at least one reload, and wait for all workers before returning.
-module(ep_concurrency_tests).

-include_lib("eunit/include/eunit.hrl").

smp_cache_encoding_test_() ->
    {timeout, 60, ?_test(run_concurrently(fun loop_encoding/1, 500000))}.

smp_cache_decoding_test_() ->
    {timeout, 60, ?_test(run_concurrently(fun loop_decoding/1, 500000))}.

run_concurrently(Loop, N) ->
    rand:uniform(),
    Defs = ep_test_helpers:person_defs(),
    Processors = erlang:system_info(logical_processors),
    Workers = [spawn_monitor(fun() ->
                  ok = ep_test_helpers:load_cache(Defs),
                  Loop(N)
              end) || _ <- lists:seq(1, Processors * 2)],
    ok = ep_test_helpers:load_cache(Defs),
    Loop(N + 1000000),
    [receive
         {'DOWN', Ref, process, Pid, Reason} -> ?assertEqual(normal, Reason)
     end || {Pid, Ref} <- Workers],
    ok.

loop_encoding(0) ->
    encoding(),
    ok;
loop_encoding(N) ->
    maybe_reload(),
    encoding(),
    loop_encoding(N - 1).

loop_decoding(0) ->
    decoding(),
    ok;
loop_decoding(N) ->
    maybe_reload(),
    decoding(),
    loop_decoding(N - 1).

maybe_reload() ->
    case rand:uniform() > 0.99 of
        true -> ok = ep_test_helpers:load_cache(ep_test_helpers:person_defs());
        false -> ok
    end.

encoding() ->
    enif_protobuf:encode({'Person', "abc def", 345, "a@example.com"}).

decoding() ->
    enif_protobuf:decode(person_wire(), 'Person').

person_wire() ->
    <<10, 7, 97, 98, 99, 32, 100, 101, 102, 16, 217, 2, 26, 13, 97, 64,
        101, 120, 97, 109, 112, 108, 101, 46, 99, 111, 109>>.
