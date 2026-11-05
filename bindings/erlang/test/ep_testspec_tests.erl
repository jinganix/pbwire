%% TESTSPEC T1-T6 for the Erlang binding, see
%% codegen/testdata/TESTSPEC.md. Every mode is asserted explicitly
%% against the shared fixtures in codegen/testdata:
%%
%%   T1  encode of the vectors-built message == pinned matrix3.bin bytes
%%   T2  decode of matrix3.bin == vectors.json, field by field
%%   T3  empty message encodes empty; explicit zero keeps presence
%%   T4  differential vs the gpb oracle: same bytes, same fields
%%   T5  one business-logic snippet runs over both codecs, same result
%%   T6  errors are loud (error class, never a crash): bad wire, bad args
%%
%% Schema source: gpb itself, never a hand-written schema. matrix3_tspec
%% is committed gpb_compile output for bindings/c/tests/data/matrix3.proto
%% (regenerate with `make tspec-gen` from bindings/erlang); the gpb
%% generated encode_msg/decode_msg/verify_msg are the differential oracle
%% (T4/T5) and pbwire:load_cache/1 consumes get_proto_defs/0 exactly as
%% gpb_compile emitted it, proto3 markers included.
%%
%% Regenerate:
%%   erl -noshell -pa _build/test/lib/gpb/ebin \
%%     -eval 'ok = gpb_compile:file("../c/tests/data/matrix3.proto", \
%%       [{module_name_suffix, "_tspec"}, {o, "test"}, \
%%        include_as_lib, introspect_get_proto_defs]), halt().'
-module(ep_testspec_tests).

-include_lib("eunit/include/eunit.hrl").
-include("matrix3_tspec.hrl").

%% ---------------------------------------------------------------------------
%% Fixtures: vectors.json (single source of truth) + pinned reference bytes.
%% ---------------------------------------------------------------------------

%% Locate a fixture under the repo checkout no matter which directory eunit
%% runs from: try each candidate path relative to every ancestor of cwd.
find_fixture([Rel | Rels]) ->
    {ok, Cwd} = file:get_cwd(),
    Dirs = [Cwd | ancestors(Cwd, 8)],
    case [{D, Rel} || D <- Dirs, filelib:is_regular(filename:join(D, Rel))] of
        [{D, _} | _] ->
            filename:join(D, Rel);
        [] ->
            find_fixture(Rels)
    end;
find_fixture([]) ->
    error(no_such_fixture).

ancestors(Dir, 0) ->
    [Dir];
ancestors(Dir, N) ->
    Parent = filename:dirname(Dir),
    case Parent =:= Dir of
        true -> [Dir];
        false -> [Dir | ancestors(Parent, N - 1)]
    end.

vectors_path() ->
    find_fixture(["codegen/testdata/vectors.json",
        "typed/testdata/vectors.json"]).

%% matrix3.bin lives next to matrix3.proto; vectors.json only records it as
%% ../../c/tests/data/matrix3.bin relative to the testdata dir, so resolve
%% it the same way as any other fixture.
reference_path() ->
    find_fixture(["bindings/c/tests/data/matrix3.bin",
        "c/tests/data/matrix3.bin"]).

load_vectors() ->
    {ok, Raw} = file:read_file(vectors_path()),
    #{<<"values">> := Values} = json:decode(Raw),
    {ok, Ref} = file:read_file(reference_path()),
    {Values, Ref}.

%% int64/uint64/fixed64 values that do not fit a float cross the JSON
%% boundary as decimal strings (TESTSPEC); Erlang holds them exactly.
int64(B) when is_binary(B) -> binary_to_integer(B);
int64(N) when is_integer(N) -> N.

%% Non-finite floats cross the JSON boundary as strings (TESTSPEC).
json_float(<<"Infinity">>) -> infinity;
json_float(<<"-Infinity">>) -> '-infinity';
json_float(<<"NaN">>) -> nan;
json_float(N) when is_number(N) -> N.

str(B) when is_binary(B) -> binary_to_list(B).

inner_from_vectors(#{<<"name">> := Name, <<"value">> := Value}) ->
    #'Inner3'{name = str(Name), value = Value}.

%% The TESTSPEC adapter: JSON values to the generated model's exact types.
scalars_from_vectors(V) ->
    #'Scalars3'{
        i32 = maps:get(<<"i32">>, V),
        i64 = int64(maps:get(<<"i64">>, V)),
        u32 = maps:get(<<"u32">>, V),
        u64 = int64(maps:get(<<"u64">>, V)),
        s32 = maps:get(<<"s32">>, V),
        s64 = int64(maps:get(<<"s64">>, V)),
        fx32 = maps:get(<<"fx32">>, V),
        fx64 = int64(maps:get(<<"fx64">>, V)),
        sfx32 = maps:get(<<"sfx32">>, V),
        sfx64 = int64(maps:get(<<"sfx64">>, V)),
        f32 = json_float(maps:get(<<"f32">>, V)),
        f64 = maps:get(<<"f64">>, V),
        flag = maps:get(<<"flag">>, V),
        str = str(maps:get(<<"str">>, V)),
        color = binary_to_atom(maps:get(<<"color">>, V)),
        inner = inner_from_vectors(maps:get(<<"inner">>, V)),
        nums = maps:get(<<"nums">>, V),
        tags = [str(T) || T <- maps:get(<<"tags">>, V)],
        inners = [inner_from_vectors(I) || I <- maps:get(<<"inners">>, V)],
        opt_i32 = maps:get(<<"opt_i32">>, V),
        opt_str = str(maps:get(<<"opt_str">>, V)),
        flags = maps:get(<<"flags">>, V)
    }.

%% One business-logic snippet (T5): total, greeting, presence, tag count.
summarize(#'Scalars3'{i32 = I32, str = Str, inner = Inner, nums = Nums,
        opt_i32 = Opt, tags = Tags}) ->
    InnerName = case Inner of
        #'Inner3'{name = N, value = V} -> {N, V};
        undefined -> {undefined, 0}
    end,
    {I32 + element(2, InnerName) + lists:sum(Nums),
        Str ++ "/" ++ element(1, InnerName),
        Opt =/= undefined,
        length(Tags)}.

load_tspec() ->
    ok = pbwire:set_opts([{string_as_list, true}, {with_utf8, true}]),
    ok = pbwire:load_cache(matrix3_tspec:get_proto_defs()).

assert_error_class(Fun) ->
    try Fun() of
        V -> error({expected_error, V})
    catch
        error:_ -> ok
    end.

%% ---------------------------------------------------------------------------
%% T1 wire: vectors-built message encodes to the pinned protoc bytes.
%% ---------------------------------------------------------------------------

t1_encode_matches_reference_bytes_test() ->
    {Values, Ref} = load_vectors(),
    %% Pin the fixture itself so silent vectors.json drift fails loudly.
    ?assertEqual(-2147483648, maps:get(<<"i32">>, Values)),
    ?assertEqual(<<"9223372036854775807">>, maps:get(<<"i64">>, Values)),
    ?assertEqual(<<"COLOR3_GREEN">>, maps:get(<<"color">>, Values)),
    ?assertEqual(-7, maps:get(<<"opt_i32">>, Values)),
    ok = load_tspec(),
    Msg = scalars_from_vectors(Values),
    ?assertEqual(Ref, pbwire:encode(Msg)),
    %% The gpb oracle agrees with the pinned bytes too.
    ?assertEqual(Ref, matrix3_tspec:encode_msg(Msg)).

%% ---------------------------------------------------------------------------
%% T2 decode: every field (including presence) matches vectors.json.
%% ---------------------------------------------------------------------------

t2_decode_matches_vectors_field_by_field_test() ->
    {Values, Ref} = load_vectors(),
    ok = load_tspec(),
    Dec = pbwire:decode(Ref, 'Scalars3'),
    Expected = scalars_from_vectors(Values),
    ?assertEqual(Expected, Dec),
    #'Scalars3'{i32 = I32, i64 = I64, u32 = U32, u64 = U64,
        s32 = S32, s64 = S64, fx32 = Fx32, fx64 = Fx64,
        sfx32 = SFx32, sfx64 = SFx64, f32 = F32, f64 = F64,
        flag = Flag, str = Str, color = Color, inner = Inner,
        nums = Nums, tags = Tags, inners = Inners,
        opt_i32 = OptI32, opt_str = OptStr, flags = Flags} = Dec,
    ?assertEqual(-2147483648, I32),
    ?assertEqual(9223372036854775807, I64),
    ?assertEqual(4294967295, U32),
    ?assertEqual(18446744073709551615, U64),
    ?assertEqual(-2147483648, S32),
    ?assertEqual(-9223372036854775808, S64),
    ?assertEqual(3735928559, Fx32),
    ?assertEqual(72623859790382856, Fx64),
    ?assertEqual(-2147483648, SFx32),
    ?assertEqual(-9223372036854775808, SFx64),
    ?assertEqual(infinity, F32),
    ?assertEqual(-123456.789, F64),
    ?assertEqual(true, Flag),
    ?assertEqual("hello", Str),
    ?assertEqual('COLOR3_GREEN', Color),
    ?assertEqual(#'Inner3'{name = "kid", value = 42}, Inner),
    ?assertEqual([1, -5, 300], Nums),
    ?assertEqual(["a", "bb"], Tags),
    ?assertEqual([#'Inner3'{name = "x", value = 1},
        #'Inner3'{name = "y", value = -2}], Inners),
    %% proto3 optional keeps explicit presence, unlike the defaulty scalars.
    ?assertEqual(-7, OptI32),
    ?assert(OptI32 =/= undefined),
    ?assertEqual("opt", OptStr),
    ?assert(OptStr =/= undefined),
    ?assertEqual([true, false, true], Flags).

%% ---------------------------------------------------------------------------
%% T3 empty/explicit-zero: empty encodes empty, explicit zero stays present
%% the way gpb models presence (undefined = absent).
%% ---------------------------------------------------------------------------

t3_empty_and_explicit_zero_test() ->
    ok = load_tspec(),
    Empty = #'Scalars3'{},
    ?assertEqual(<<>>, matrix3_tspec:encode_msg(Empty)),
    ?assertEqual(<<>>, pbwire:encode(Empty)),
    ?assertEqual(Empty, pbwire:decode(<<>>, 'Scalars3')),
    %% Explicit zero on a proto3 optional field is sent (presence bit set).
    ZeroOpt = #'Scalars3'{opt_i32 = 0},
    ?assertEqual(<<160, 1, 0>>, matrix3_tspec:encode_msg(ZeroOpt)),
    ?assertEqual(<<160, 1, 0>>, pbwire:encode(ZeroOpt)),
    #'Scalars3'{opt_i32 = 0} = pbwire:decode(<<160, 1, 0>>, 'Scalars3'),
    #'Scalars3'{opt_i32 = 0} =
        matrix3_tspec:decode_msg(<<160, 1, 0>>, 'Scalars3'),
    %% Absent stays absent: undefined encodes to nothing on both codecs.
    AbsentOpt = #'Scalars3'{opt_i32 = undefined, opt_str = undefined},
    ?assertEqual(<<>>, matrix3_tspec:encode_msg(AbsentOpt)),
    ?assertEqual(<<>>, pbwire:encode(AbsentOpt)),
    %% An explicitly empty optional string is still sent (presence), and
    %% both codecs decode it back the same way.
    EmptyOptStr = #'Scalars3'{opt_str = ""},
    ?assertEqual(matrix3_tspec:encode_msg(EmptyOptStr),
        pbwire:encode(EmptyOptStr)),
    ?assertEqual(matrix3_tspec:decode_msg(
        matrix3_tspec:encode_msg(EmptyOptStr), 'Scalars3'),
        pbwire:decode(pbwire:encode(EmptyOptStr), 'Scalars3')).

%% ---------------------------------------------------------------------------
%% T4 differential: gpb and pbwire agree on bytes and on fields.
%% ---------------------------------------------------------------------------

t4_gpb_parity_bytes_and_fields_test() ->
    {Values, Ref} = load_vectors(),
    ok = load_tspec(),
    Msg = scalars_from_vectors(Values),
    GpbBin = matrix3_tspec:encode_msg(Msg),
    EpbBin = pbwire:encode(Msg),
    ?assertEqual(Ref, GpbBin),
    ?assertEqual(Ref, EpbBin),
    GpbMsg = matrix3_tspec:decode_msg(Ref, 'Scalars3'),
    EpbMsg = pbwire:decode(Ref, 'Scalars3'),
    ?assertEqual(GpbMsg, EpbMsg),
    %% Cross-decode: each codec reads the other's bytes to the same record.
    ?assertEqual(Msg, matrix3_tspec:decode_msg(EpbBin, 'Scalars3')),
    ?assertEqual(Msg, pbwire:decode(GpbBin, 'Scalars3')),
    %% Canonicalization spot checks: enum atom <-> number, exact int64.
    ?assertEqual(2, matrix3_tspec:enum_value_by_symbol('Color3',
        EpbMsg#'Scalars3'.color)),
    ?assertEqual('COLOR3_GREEN',
        matrix3_tspec:enum_symbol_by_value('Color3', 2)),
    ?assertEqual(9223372036854775807, EpbMsg#'Scalars3'.i64),
    ?assertEqual(18446744073709551615, EpbMsg#'Scalars3'.u64).

%% ---------------------------------------------------------------------------
%% T5 switch: identical business code over both codecs, identical results.
%% ---------------------------------------------------------------------------

t5_same_business_code_over_both_codecs_test() ->
    {Values, Ref} = load_vectors(),
    ok = load_tspec(),
    Expected = summarize(scalars_from_vectors(Values)),
    %% Same snippet over gpb-decoded and pbwire-decoded reference bytes.
    ?assertEqual(Expected,
        summarize(matrix3_tspec:decode_msg(Ref, 'Scalars3'))),
    ?assertEqual(Expected, summarize(pbwire:decode(Ref, 'Scalars3'))),
    %% Same snippet over each codec's own round-trip.
    GpbRoundTrip = matrix3_tspec:decode_msg(
        matrix3_tspec:encode_msg(scalars_from_vectors(Values)),
        'Scalars3'),
    EpbRoundTrip = pbwire:decode(pbwire:encode(scalars_from_vectors(Values)),
        'Scalars3'),
    ?assertEqual(summarize(GpbRoundTrip), summarize(EpbRoundTrip)),
    ?assertEqual(Expected, summarize(EpbRoundTrip)).

%% ---------------------------------------------------------------------------
%% T6 errors: verify/encode reject bad values alike, bad wire and bad args
%% raise errors (loud, never a VM crash).
%% ---------------------------------------------------------------------------

t6_bad_wire_raises_on_both_codecs_test() ->
    {_Values, Ref} = load_vectors(),
    ok = load_tspec(),
    N = byte_size(Ref) - 3,
    <<Truncated:N/binary, _/binary>> = Ref,
    assert_error_class(fun() -> pbwire:decode(Truncated, 'Scalars3') end),
    assert_error_class(
        fun() -> matrix3_tspec:decode_msg(Truncated, 'Scalars3') end),
    %% Overlong garbage tag: pbwire raises instead of misreading the wire.
    assert_error_class(
        fun() -> pbwire:decode(<<255, 255, 255, 255, 255,
            255, 255, 255, 255, 127>>, 'Scalars3') end).

t6_bad_args_raise_named_errors_test() ->
    ok = load_tspec(),
    ?assertError(badarg, pbwire:encode(not_a_tuple)),
    ?assertError(badarg, pbwire:decode(not_a_binary, 'Scalars3')),
    ?assertError(badarg, pbwire:decode(<<>>, "Scalars3")),
    %% Unknown message: the NIF raises the name itself as the error.
    ?assertError('Nope', pbwire:decode(<<8, 1>>, 'Nope')),
    ok = pbwire:purge_cache(),
    try
        ?assertError(cache_not_exists, pbwire:encode(#'Scalars3'{})),
        ?assertError(cache_not_exists, pbwire:decode(<<>>, 'Scalars3'))
    after
        load_tspec()
    end.

t6_verify_and_encode_reject_bad_values_alike_test() ->
    ok = load_tspec(),
    %% Wrong field type: verify rejects, pbwire encode rejects, both loud.
    BadType = #'Scalars3'{i32 = "not-an-int"},
    assert_error_class(
        fun() -> matrix3_tspec:verify_msg(BadType, 'Scalars3') end),
    assert_error_class(fun() -> pbwire:encode(BadType) end),
    %% Out-of-range int32: both reject instead of silently masking.
    BigRange = #'Scalars3'{i32 = 1 bsl 40},
    assert_error_class(
        fun() -> matrix3_tspec:verify_msg(BigRange, 'Scalars3') end),
    assert_error_class(fun() -> pbwire:encode(BigRange) end),
    %% Unknown enum value: both codecs agree it decodes to the integer.
    ?assertEqual(matrix3_tspec:decode_msg(<<120, 99>>, 'Scalars3'),
        pbwire:decode(<<120, 99>>, 'Scalars3')),
    #'Scalars3'{color = 99} = pbwire:decode(<<120, 99>>, 'Scalars3').

%% ---------------------------------------------------------------------------
%% Toolchain pins: record which oracle versions this suite ran against.
%% ---------------------------------------------------------------------------

toolchain_versions_test() ->
    GpbVsn = matrix3_tspec:gpb_version_as_string(),
    OtpVsn = erlang:system_info(otp_release),
    ?debugFmt("TESTSPEC oracle: gpb ~s, otp ~s", [GpbVsn, OtpVsn]),
    ?assert(is_list(GpbVsn) andalso GpbVsn =/= []),
    ?assert(is_list(OtpVsn) andalso OtpVsn =/= []).
