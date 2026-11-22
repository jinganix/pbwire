# frozen_string_literal: true

# The one payload every Ruby pbench arm times, and the official
# google-protobuf object it has to match: structtest.Scalars3 from
# bindings/c/tests/data/matrix3.proto scaled over the four standard tiers.
#
# The native object is a generated model instance (Pbwire::Gen), the shape a
# user of the generated facade holds: proto field names as accessors, nested
# models and Arrays for repeated fields. The oracle is the google-protobuf
# gem's generated class from the same .proto.

require 'google/protobuf'
require_relative '../test/gen_oracle/matrix3_pb'
require_relative '../test/gen/matrix3'
require_relative '../lib/pbwire'

module Payload
  TIERS = [
    { label: 'small',  repeats: 0,    bytes: 136,   iterations: 200_000 },
    { label: 'medium', repeats: 1,    bytes: 174,   iterations: 200_000 },
    { label: 'large',  repeats: 100,  bytes: 2469,  iterations: 20_000 },
    { label: 'xlarge', repeats: 3000, bytes: 74_913, iterations: 2_000 }
  ].freeze

  # The generated model classes, keyed by proto full name. decode_object
  # needs this map because a descriptor only names types; the class choice is
  # the host's.
  CLASSES = {
    'structtest.Scalars3' => Pbwire::Gen::Structtest_Scalars3,
    'structtest.Inner3' => Pbwire::Gen::Structtest_Inner3
  }.freeze

  module_function

  # The Scalars3 type handle of the generated model's embedded descriptor.
  def type
    @type ||= Pbwire::Gen._type(Pbwire::Gen::DESC_B64_MATRIX3,
                                'structtest.Scalars3')
  end

  # The native object for `repeats` scaled elements. Every field is present
  # (the matrix3 payload uses non-zero values on purpose), including the two
  # proto3 optionals.
  def object(repeats)
    Pbwire::Gen::Structtest_Scalars3.new(
      i32: -2_147_483_648,
      i64: 9_223_372_036_854_775_807,
      u32: 4_294_967_295,
      u64: 18_446_744_073_709_551_615,
      s32: -2_147_483_648,
      s64: -9_223_372_036_854_775_808,
      fx32: 3_735_928_559,
      fx64: 72_623_859_790_382_856,
      sfx32: -2_147_483_648,
      sfx64: -9_223_372_036_854_775_808,
      f32: Float::INFINITY,
      f64: -123_456.789,
      flag: true,
      str: 'hello',
      color: 2,
      inner: inner('kid', 42),
      nums: Array.new(repeats) { |i| i - 3 },
      tags: Array.new(repeats) { 'tag' },
      inners: Array.new(repeats) { |i| inner('grandkid', i + 1) },
      opt_i32: -7,
      opt_str: 'opt',
      flags: Array.new(repeats) { |i| i.even? }
    )
  end

  def inner(name, value)
    Pbwire::Gen::Structtest_Inner3.new(name: name, value: value)
  end

  # The official (google-protobuf) object for `repeats`: the bytes every arm
  # must reproduce.
  def official(repeats)
    m = Structtest::Scalars3.new
    m.i32 = -2_147_483_648
    m.i64 = 9_223_372_036_854_775_807
    m.u32 = 4_294_967_295
    m.u64 = 18_446_744_073_709_551_615
    m.s32 = -2_147_483_648
    m.s64 = -9_223_372_036_854_775_808
    m.fx32 = 3_735_928_559
    m.fx64 = 72_623_859_790_382_856
    m.sfx32 = -2_147_483_648
    m.sfx64 = -9_223_372_036_854_775_808
    m.f32 = Float::INFINITY
    m.f64 = -123_456.789
    m.flag = true
    m.str = 'hello'
    m.color = :COLOR3_GREEN
    m.inner = Structtest::Inner3.new(name: 'kid', value: 42)
    m.opt_i32 = -7
    m.opt_str = 'opt'
    repeats.times do |i|
      m.nums << i - 3
      m.tags << 'tag'
      m.inners << Structtest::Inner3.new(name: 'grandkid', value: i + 1)
      m.flags << i.even?
    end
    m
  end

  def official_encode(repeats)
    Structtest::Scalars3.encode(official(repeats))
  end

  # The tree arm's whole user task: build the model, then encode it.
  def tree_encode(obj)
    Pbwire::Gen::Structtest_Scalars3.encode(obj)
  end

  # The tree arm's read side: decode, then read every field back.
  def tree_decode(wire)
    Pbwire::Gen::Structtest_Scalars3.decode(wire)
  end

  # Reads every field the benchmarks check, on any arm's decoded object.
  def reads_ok?(back, repeats)
    back.str == 'hello' &&
      back.inner.name == 'kid' &&
      back.inner.value == 42 &&
      back.nums.length == repeats &&
      back.tags.length == repeats &&
      back.inners.length == repeats &&
      back.flags.length == repeats &&
      (repeats.zero? ||
        (back.nums.last == repeats - 4 &&
         back.tags.last == 'tag' &&
         back.inners.last.name == 'grandkid' &&
         back.inners.last.value == repeats &&
         back.flags.last == (repeats - 1).even?)) &&
      back.opt_i32 == -7 &&
      back.opt_str == 'opt' &&
      back.has_opt_i32? &&
      back.has_opt_str? &&
      back.u64 == 18_446_744_073_709_551_615 &&
      back.color == 2
  end
end
