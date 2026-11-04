# frozen_string_literal: true

# Ruby binding benchmark against the official google-protobuf gem on the same
# matrix3.proto and the four standard tiers from BENCHMARKS.md: every scalar, the
# nested message, the proto3 optionals, and the repeated fields scaled
# x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes).
#
# Both sides build the same tree and must produce identical bytes before
# anything is timed.
#
#   ./bench.sh          generate, build, run

$LOAD_PATH.unshift File.expand_path('gen', __dir__)
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)

require 'google/protobuf'
require 'matrix3_pb'
require 'pbwire'

CASES = [
  ['small',  0,     136,   200_000],
  ['medium', 1,     174,   200_000],
  ['large',  100,   2469,  20_000],
  ['xlarge', 3000,  74_913, 2_000]
].freeze

$failures = 0

def fill_official(repeats)
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

def inner_of(schema, name, value)
  inner = schema.message('structtest.Inner3')
  inner['name'] = name
  inner['value'] = value
  inner
end

def fill_pbwire(schema, repeats)
  m = schema.message('structtest.Scalars3')
  m['i32'] = -2_147_483_648
  m['i64'] = 9_223_372_036_854_775_807
  m['u32'] = 4_294_967_295
  m['u64'] = 18_446_744_073_709_551_615
  m['s32'] = -2_147_483_648
  m['s64'] = -9_223_372_036_854_775_808
  m['fx32'] = 3_735_928_559
  m['fx64'] = 72_623_859_790_382_856
  m['sfx32'] = -2_147_483_648
  m['sfx64'] = -9_223_372_036_854_775_808
  m['f32'] = Float::INFINITY
  m['f64'] = -123_456.789
  m['flag'] = true
  m['str'] = 'hello'
  m['color'] = 2
  m['inner'] = inner_of(schema, 'kid', 42)
  m['opt_i32'] = -7
  m['opt_str'] = 'opt'
  m['nums'] = Array.new(repeats) { |i| i - 3 }
  m['tags'] = Array.new(repeats) { 'tag' }
  m['inners'] = Array.new(repeats) { |i| inner_of(schema, 'grandkid', i + 1) }
  m['flags'] = Array.new(repeats) { |i| i.even? }
  m
end

def ns_per_op(iterations)
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  iterations.times { yield }
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started) * 1e9 / iterations
end

# One warm-up pass, then three interleaved passes; the best run per operation
# wins. Interleaving keeps a busy machine from biasing one implementation.
def measure(iterations, ops)
  ops.each_value { |op| ns_per_op(iterations, &op) }
  best = {}
  3.times do
    ops.each do |name, op|
      ns = ns_per_op(iterations, &op)
      best[name] = ns if best[name].nil? || ns < best[name]
    end
  end
  best
end

def row(impl, op, bytes, ns)
  format("  %-14s %-7s %12.1f %10.1f\n", impl, op, ns, bytes / ns * 1000)
end

def fail(label, what)
  puts "FAIL #{label}: #{what}"
  $failures += 1
end

schema = Pbwire::Schema.from_file(File.expand_path('gen/matrix3.desc', __dir__))

CASES.each do |label, repeats, want, iterations|
  reference = Structtest::Scalars3.encode(fill_official(repeats))
  if reference.bytesize != want
    fail(label, "official encoder wrote #{reference.bytesize} bytes, expected #{want}")
    next
  end
  message = fill_pbwire(schema, repeats)
  unless message.encode == reference
    fail(label, 'pbwire-ruby and google-protobuf encode differently')
    next
  end
  puts "OK   #{label} encoding parity (#{reference.bytesize} bytes)"

  official = fill_official(repeats)
  decoder = schema.message('structtest.Scalars3')
  ns = measure(iterations,
               'pbwire_encode' => -> { message.encode },
               'gem_encode' => -> { Structtest::Scalars3.encode(official) },
               'pbwire_decode' => -> { decoder.decode(reference).close },
               'gem_decode' => -> { Structtest::Scalars3.decode(reference) })
  encode_ns = ns['pbwire_encode']
  decode_ns = ns['pbwire_decode']
  official_encode_ns = ns['gem_encode']
  official_decode_ns = ns['gem_decode']

  puts "#{label}: #{reference.bytesize} bytes, #{iterations} iterations " \
       "(repeated fields x#{repeats})"
  puts format("  %-14s %-7s %12s %10s", 'impl', 'op', 'ns/op', 'MB/s')
  puts row('pbwire-ruby', 'encode', reference.bytesize, encode_ns)
  puts row('google-protobuf', 'encode', reference.bytesize, official_encode_ns)
  puts row('pbwire-ruby', 'decode', reference.bytesize, decode_ns)
  puts row('google-protobuf', 'decode', reference.bytesize, official_decode_ns)
  puts
end

abort("#{$failures} check(s) failed") unless $failures.zero?
