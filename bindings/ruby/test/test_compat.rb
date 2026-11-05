# frozen_string_literal: true

# Typed-compat check for the Ruby binding (TESTSPEC T1–T6, vectors.json as
# the single source of truth):
#
#   T1  generated encode == pinned protoc reference bytes
#   T2  generated decode of the reference == vectors, field by field
#   T3  empty encodes empty; explicit zero with presence round-trips
#   T4  differential vs the pinned google-protobuf gem: same bytes, same
#       to_h, has_? agrees
#   T5  one business snippet runs unchanged against the gem's classes and
#       ours (import swap only) with identical observable results
#   T6  errors are loud, not crashes: bad assignment, bad wire, oneof
#
#   RUBY=/opt/homebrew/bin/ruby ruby -Ibindings/ruby/lib bindings/ruby/test/test_compat.rb
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'
require 'json'
require 'base64'

$LOAD_PATH.unshift File.expand_path('gen', __dir__)
$LOAD_PATH.unshift File.expand_path('gen_oracle', __dir__)
require 'matrix3'
require 'oneof1'

ORACLE_PIN = '4.36.2'

TESTDATA = File.expand_path('../../typed/testdata', __dir__)
VECTORS = JSON.parse(File.read(File.join(TESTDATA, 'vectors.json')))

CHECKS = [0]

def check(ok, what)
  raise "FAIL #{what}" unless ok

  CHECKS[0] += 1
end

def ref_bin(rel)
  File.binread(File.join(TESTDATA, rel))
end

SCALARS = Pbwire::Gen::Structtest_Scalars3
INNER = Pbwire::Gen::Structtest_Inner3

def build_scalars
  v = VECTORS['values']
  SCALARS.new(
    i32: v['i32'], i64: v['i64'].to_i, u32: v['u32'], u64: v['u64'].to_i,
    s32: v['s32'], s64: v['s64'].to_i, fx32: v['fx32'], fx64: v['fx64'].to_i,
    sfx32: v['sfx32'], sfx64: v['sfx64'].to_i, f32: Float::INFINITY, f64: v['f64'],
    flag: v['flag'], str: v['str'], color: Pbwire::Gen::Structtest_Color3::COLOR3_GREEN,
    inner: INNER.new(name: v['inner']['name'], value: v['inner']['value']),
    nums: v['nums'], tags: v['tags'],
    inners: v['inners'].map { |x| INNER.new(name: x['name'], value: x['value']) },
    opt_i32: v['opt_i32'], has_opt_i32: true,
    opt_str: v['opt_str'], has_opt_str: true,
    flags: v['flags']
  )
end

def check_t1_t2_t3
  reference = ref_bin(VECTORS['reference']['bin'])
  check(SCALARS.encode(build_scalars) == reference, 'T1 generated encode == protoc reference bytes')

  back = SCALARS.decode(reference)
  v = VECTORS['values']
  check(back.i32 == v['i32'], 'i32')
  check(back.i64 == v['i64'].to_i, 'i64')
  check(back.u32 == v['u32'], 'u32')
  check(back.u64 == v['u64'].to_i, 'u64')
  check(back.s32 == v['s32'], 's32')
  check(back.s64 == v['s64'].to_i, 's64')
  check(back.fx32 == v['fx32'], 'fx32')
  check(back.fx64 == v['fx64'].to_i, 'fx64')
  check(back.sfx32 == v['sfx32'], 'sfx32')
  check(back.sfx64 == v['sfx64'].to_i, 'sfx64')
  check(back.f32 == Float::INFINITY, 'f32')
  check(back.f64 == v['f64'], 'f64')
  check(back.flag == v['flag'], 'flag')
  check(back.str == v['str'], 'str')
  check(back.color == 2, 'color')
  check(back.inner.name == 'kid' && back.inner.value == 42, 'inner')
  check(back.nums == v['nums'], 'nums')
  check(back.tags == v['tags'], 'tags')
  check(back.inners.map(&:to_h) == [{ name: 'x', value: 1 }, { name: 'y', value: -2 }], 'inners')
  check(back.opt_i32 == v['opt_i32'] && back.has_opt_i32?, 'opt_i32 presence')
  check(back.opt_str == v['opt_str'] && back.has_opt_str?, 'opt_str presence')
  check(back.flags == v['flags'], 'flags')

  check(SCALARS.new.to_proto == '', 'T3 empty encodes empty')
  zero = SCALARS.decode(SCALARS.new(has_opt_i32: true, opt_i32: 0).to_proto)
  check(zero.opt_i32.zero? && zero.has_opt_i32?, 'T3 explicit zero round-trips')

  extra = VECTORS['extra_messages'][0]
  ref1 = ref_bin(extra['reference']['bin'])
  oneof = Pbwire::Gen::Spectest_Oneof1
  check(oneof.encode(oneof.new(name: 'a', text: 'hi')) == ref1, 'T1 oneof encode == reference')
  back1 = oneof.decode(ref1)
  check(back1.name == 'a' && back1.text == 'hi', 'T2 oneof decode')
  nested = oneof.decode(oneof.new(name: 'n', inner: Pbwire::Gen::Spectest_Oneof1_Inner.new(v: 7)).to_proto)
  check(nested.inner.v == 7, 'T2 nested oneof member')
end

def check_t4
  require 'google/protobuf'
  require 'matrix3_pb'
  require 'oneof1_pb'

  got = Gem.loaded_specs['google-protobuf'].version.to_s
  check(got == ORACLE_PIN, "oracle pin drifted (got #{got}); see testdata/ORACLES.md")

  reference = ref_bin(VECTORS['reference']['bin'])
  v = VECTORS['values']
  pb2 = Structtest::Scalars3.new(
    i32: v['i32'], i64: v['i64'].to_i, u32: v['u32'], u64: v['u64'].to_i,
    s32: v['s32'], s64: v['s64'].to_i, fx32: v['fx32'], fx64: v['fx64'].to_i,
    sfx32: v['sfx32'], sfx64: v['sfx64'].to_i, f32: Float::INFINITY, f64: v['f64'],
    flag: v['flag'], str: v['str'], color: :COLOR3_GREEN,
    inner: Structtest::Inner3.new(name: v['inner']['name'], value: v['inner']['value']),
    nums: v['nums'], tags: v['tags'],
    inners: v['inners'].map { |x| Structtest::Inner3.new(name: x['name'], value: x['value']) },
    opt_i32: v['opt_i32'], opt_str: v['opt_str'], flags: v['flags']
  )
  check(Structtest::Scalars3.encode(pb2) == reference, 'T4 oracle matches the reference')
  check(SCALARS.encode(build_scalars) == reference, 'T4 facade matches the reference')
  check(SCALARS.decode(reference).to_h == pb2.to_h, 'T4 to_h agrees')

  ours = SCALARS.decode(reference)
  check(ours.has_opt_i32? == pb2.has_opt_i32?, 'T4 has_? agrees on opt_i32')
  check(ours.has_opt_str? == pb2.has_opt_str?, 'T4 has_? agrees on opt_str')

  extra = VECTORS['extra_messages'][0]
  ref1 = ref_bin(extra['reference']['bin'])
  check(Spectest::Oneof1.encode(Spectest::Oneof1.new(name: 'a', text: 'hi')) == ref1, 'T4 oracle oneof')
  check(Pbwire::Gen::Spectest_Oneof1.encode(
    Pbwire::Gen::Spectest_Oneof1.new(name: 'a', text: 'hi')) == ref1, 'T4 facade oneof')
end

def check_t5
  # One business snippet, two libraries: build, check presence, serialize,
  # parse back, read fields. The require is the only difference.
  workflow = lambda do |scalars, inner|
    m = scalars.new(str: 'kid', i32: -5, nums: [1, 2], inner: inner.new(name: 'x', value: 1))
    wire = scalars.encode(m)
    back = scalars.decode(wire)
    [wire, back.has_inner?, back.str, back.i32, back.nums.to_a, back.inner.name, back.to_h]
  end

  a = workflow.call(Structtest::Scalars3, Structtest::Inner3)
  b = workflow.call(SCALARS, INNER)
  check(a == b, 'T5 same snippet, same observable result (import swap only)')
end

def check_t6
  # Ours validates at the encode boundary (plain Ruby objects until then);
  # the gem validates at construction. Both raise TypeError, loudly.
  begin
    SCALARS.encode(SCALARS.new(str: 42))
    check(false, 'T6 bad value raises')
  rescue TypeError
    check(true, 'T6 bad value raises')
  end
  begin
    SCALARS.decode('garbage!!!')
    check(false, 'T6 bad wire raises')
  rescue Pbwire::Error
    check(true, 'T6 bad wire raises')
  end
  begin
    Structtest::Scalars3.new(str: 42)
    check(false, 'T6 oracle bad assignment raises')
  rescue Google::Protobuf::TypeError
    check(true, 'T6 oracle bad assignment raises')
  end
  m = build_scalars
  check(m.has_opt_i32?, 'T6 presence before clear')
  check(!SCALARS.new.has_opt_i32?, 'T6 absent presence reads false')
  oneof = Pbwire::Gen::Spectest_Oneof1
  both = oneof.new(name: 'n', num: 1, text: 'x')
  check(both.to_h == { name: 'n', text: 'x' }, 'T6 oneof last member wins')
end

check_t1_t2_t3
check_t4
check_t5
check_t6
puts "test_compat: #{CHECKS[0]} checks passed"
