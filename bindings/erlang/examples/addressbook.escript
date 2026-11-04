#!/usr/bin/env escript
%% -*- erlang -*-
%%! -pa _build/test/lib/gpb/ebin -pa _build/default/lib/pbwire/ebin
%%
%% AddressBook example (Erlang): hand-built gpb-style schema, then a plain
%% record round-trips through pbwire.
%%
%% Run from bindings/erlang (needs OTP, the test profile and gpb headers):
%%
%%     ./scripts/with_tool_versions.sh escript examples/addressbook.escript
%%
%% The defs are the shape gpb_compile emits (and gpb.hrl documents); a real
%% project generates them from a .proto instead of writing them by hand, and
%% uses the generated records. Here the messages are plain tuples in rnum
%% order (element 1 is the message name), which is what a record compiles to.
-mode(compile).
-include_lib("gpb/include/gpb.hrl").
-export([main/1]).

defs() ->
    [{{msg, 'Person'}, [
        #field{name = name, fnum = 1, rnum = 2, type = string,
               occurrence = required, opts = []},
        #field{name = id, fnum = 2, rnum = 3, type = int32,
               occurrence = required, opts = []},
        #field{name = email, fnum = 3, rnum = 4, type = string,
               occurrence = optional, opts = []},
        #field{name = phones, fnum = 4, rnum = 5, type = {msg, 'Person.PhoneNumber'},
               occurrence = repeated, opts = []}
    ]},
     {{msg, 'Person.PhoneNumber'}, [
        #field{name = number, fnum = 1, rnum = 2, type = string,
               occurrence = optional, opts = []},
        #field{name = type, fnum = 2, rnum = 3, type = {enum, 'Person.PhoneType'},
               occurrence = optional, opts = []}
    ]},
     {{enum, 'Person.PhoneType'}, [{'MOBILE', 0}, {'HOME', 1}, {'WORK', 2}]}].

main(_Args) ->
    ok = pbwire:load_cache(defs()),
    Person = {'Person', <<"alice">>, 1234, undefined,
              [{'Person.PhoneNumber', <<"555-1234">>, 'MOBILE'}]},
    Wire = pbwire:encode(Person),
    Person = pbwire:decode(Wire, 'Person'),
    {'Person', Name, Id, _Email, Phones} = Person,
    io:format("~w bytes, name=~s, id=~w, ~w phone(s)~n",
              [byte_size(Wire), Name, Id, length(Phones)]).
