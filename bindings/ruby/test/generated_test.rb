# frozen_string_literal: true

# Generated-conformance check for the Ruby binding: build matrix3.proto's Scalars3
# through the generated classes, encode, require the protoc reference bytes
# (matrix3.bin), then decode those bytes back and check every field.
#
#   ruby -Ibindings/ruby/lib bindings/ruby/test/generated_test.rb

$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'
require_relative 'gen/matrix3'
require_relative 'gen/xfile'

DATA = File.expand_path('../../c/tests/data', __dir__)
REFERENCE = File.binread(File.join(DATA, 'matrix3.bin'))
CHECKS = [0]

def check(ok, what)
  raise "FAIL #{what}" unless ok

  CHECKS[0] += 1
end

def build
  kid = Pbwire::Gen::Structtest_Inner3.new(name: 'kid', value: 42)
  x = Pbwire::Gen::Structtest_Inner3.new(name: 'x', value: 1)
  y = Pbwire::Gen::Structtest_Inner3.new(name: 'y', value: -2)
  Pbwire::Gen::Structtest_Scalars3.new(
    i32: -2_147_483_648, i64: 9_223_372_036_854_775_807,
    u32: 4_294_967_295, u64: 18_446_744_073_709_551_615,
    s32: -2_147_483_648, s64: -9_223_372_036_854_775_808,
    fx32: 3_735_928_559, fx64: 72_623_859_790_382_856,
    sfx32: -2_147_483_648, sfx64: -9_223_372_036_854_775_808,
    f32: Float::INFINITY, f64: -123_456.789,
    flag: true, str: 'hello', color: Pbwire::Gen::Structtest_Color3::COLOR3_GREEN,
    inner: kid, nums: [1, -5, 300], tags: %w[a bb], inners: [x, y],
    opt_i32: -7, has_opt_i32: true, opt_str: 'opt', has_opt_str: true,
    flags: [true, false, true]
  )
end

def verify(m)
  check(m.i32 == -2_147_483_648, 'i32')
  check(m.i64 == 9_223_372_036_854_775_807, 'i64')
  check(m.u32 == 4_294_967_295, 'u32')
  check(m.u64 == 18_446_744_073_709_551_615, 'u64')
  check(m.s32 == -2_147_483_648, 's32')
  check(m.s64 == -9_223_372_036_854_775_808, 's64')
  check(m.fx32 == 3_735_928_559, 'fx32')
  check(m.fx64 == 72_623_859_790_382_856, 'fx64')
  check(m.sfx32 == -2_147_483_648, 'sfx32')
  check(m.sfx64 == -9_223_372_036_854_775_808, 'sfx64')
  check(m.f32 == Float::INFINITY, 'f32')
  check(m.f64 == -123_456.789, 'f64')
  check(m.flag == true, 'flag')
  check(m.str == 'hello', 'str')
  check(m.color == Pbwire::Gen::Structtest_Color3::COLOR3_GREEN, 'color')
  check(m.inner.name == 'kid' && m.inner.value == 42, 'inner')
  check(m.nums == [1, -5, 300], 'nums')
  check(m.tags == %w[a bb], 'tags')
  check(m.inners.length == 2 && m.inners[1].value == -2, 'inners')
  check(m.has_opt_i32 && m.opt_i32 == -7, 'opt_i32')
  check(m.has_opt_str && m.opt_str == 'opt', 'opt_str')
  check(m.flags == [true, false, true], 'flags')
end

schema = Pbwire::Schema.from_file(File.join(DATA, 'matrix3.desc'))
scalars = Pbwire::Gen::Structtest_Scalars3
wire = build.encode(schema)
check(wire == REFERENCE, 'generated encode equals the protoc reference bytes')
verify(scalars.decode(schema, REFERENCE))
check(scalars.new.encode(schema).empty?, 'empty generated message is empty')

zero = scalars.new(has_opt_i32: true, opt_i32: 0)
zero_back = scalars.decode(schema, zero.encode(schema))
check(zero_back.has_opt_i32 && zero_back.opt_i32.zero?, 'explicit zero presence round-trips')

nested = scalars.new(inner: Pbwire::Gen::Structtest_Inner3.new(name: 'kid', value: 42))
check(scalars.decode(schema, nested.encode(schema)).inner.value == 42, 'nested generated message')

inner_wire = Pbwire::Gen::Structtest_Inner3.new(name: 'kid').encode(schema)
check(Pbwire::Gen::Structtest_Inner3.decode(schema, inner_wire).name == 'kid', 'inner encodes standalone')

xdata = File.expand_path('../../../codegen/testdata', __dir__)
xreference = File.binread(File.join(xdata, 'xfile.bin'))
xschema = Pbwire::Schema.from_file(File.join(xdata, 'xfile.desc'))
xuses = Pbwire::Gen::Xfile_UsesDep
xbuilt = xuses.new(
  single: Pbwire::Gen::Xdep_DepMsg.new(name: 'one', value: 7),
  many: [Pbwire::Gen::Xdep_DepMsg.new(name: 'a'),
         Pbwire::Gen::Xdep_DepMsg.new(name: 'b', value: -1)],
  note: 'hi'
)
check(xbuilt.encode(xschema) == xreference, 'xfile encode equals the protoc reference bytes')
xback = xuses.decode(xschema, xreference)
check(xback.single.name == 'one' && xback.single.value == 7, 'xfile single')
check(xback.many.map(&:name) == %w[a b] && xback.many[1].value == -1, 'xfile many')
check(xback.note == 'hi', 'xfile note')
xschema.close

schema.close
puts "generated_test: #{CHECKS[0]} checks passed"
