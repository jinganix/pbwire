# frozen_string_literal: true

# Ruby binding benchmark against the official google-protobuf gem on the same
# matrix3.proto and the four standard tiers from BENCHMARKS.md: every scalar, the
# nested message, the proto3 optionals, and the repeated fields scaled
# x0 / x1 / x100 / x3000 (136 / 174 / 2469 / 74913 bytes).
#
# Four pbwire rows, named by the mechanism they time. The first two time the
# same native-in/native-out task; the third is the prebuilt handle:
#
#   pbwire-ruby-tree   a native model in and out through the shipped
#                        per-field facade: Structtest_Scalars3.encode(obj)
#                        builds a Message with one set per field, then encodes;
#                        .decode(wire) reads one get per field back into a fresh
#                        model (the pbench a-tree arm). The model is built fresh
#                        per iteration and every gate-checked field is read back
#                        inside the timer. No prebuilt handle before the timer.
#   pbwire-ruby-shim   the same model in and out in one C-extension entry per
#                        op: encode_object walks the model's ivars in C,
#                        decode_object materializes the models in C. Encode
#                        builds a fresh model per iteration inside the timer
#                        (message preparation included), on both sides; decode
#                        materializes a fresh
#                        model per call inside the timer. Shim decode carries
#                        no Ruby-side folding sink by design: type_decode_object
#                        (bindings/ruby/ext/pbwire_ext/pbwire_ext.c) always runs
#                        pbw_decode plus object_from_tree over every field
#                        before it returns -- repeated arrays and nested models
#                        included -- so the full Ruby object already exists when
#                        the timer lambda drops the reference. A consume over
#                        the result would only add Ruby getter calls on top of
#                        finished C work; it cannot fuse or skip anything.
#   pbwire-ruby-proxy  encode/decode of a Message handle built before the
#                        timer: re-encode a held handle, decode-and-free with no
#                        materialization. The codec's own cost once the tree
#                        exists. Published, never the headline.
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
require_relative '../test/gen/matrix3'

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

MODEL_CLASSES = {
  'structtest.Scalars3' => Pbwire::Gen::Structtest_Scalars3,
  'structtest.Inner3' => Pbwire::Gen::Structtest_Inner3
}.freeze

# The generated model, the native object the shim row hands to the extension.
def model_inner(name, value)
  Pbwire::Gen::Structtest_Inner3.new(name: name, value: value)
end

def fill_model(repeats)
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
    inner: model_inner('kid', 42),
    nums: Array.new(repeats) { |i| i - 3 },
    tags: Array.new(repeats) { 'tag' },
    inners: Array.new(repeats) { |i| model_inner('grandkid', i + 1) },
    opt_i32: -7,
    opt_str: 'opt',
    flags: Array.new(repeats) { |i| i.even? }
  )
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

# Floor iterations per tier; doubled until one pass takes 0.25 s (shared
# standard, see BENCHMARKS.md). The count is calibrated on the binding's own
# encode; slower ops then take longer than the minimum.
MIN_PASS_SECONDS = 0.25

def calibrate(op, floor)
  count = [1, floor].max
  loop do
    started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    count.times { op.call }
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    return count if elapsed >= MIN_PASS_SECONDS || count >= 100_000_000
    count *= 2
  end
end

# Every op calibrates independently from the floor (a slow op already exceeds
# the minimum at the floor and keeps it), then one warm-up pass and three
# interleaved passes; the best run per operation wins. Interleaving keeps a
# busy machine from biasing one implementation. Returns [best, ref_count].
def measure(iterations, ops)
  counts = {}
  ops.each do |name, op|
    counts[name] = calibrate(op, iterations)
  end
  ops.each { |name, op| ns_per_op(counts[name], &op) }
  best = {}
  3.times do
    ops.each do |name, op|
      ns = ns_per_op(counts[name], &op)
      best[name] = ns if best[name].nil? || ns < best[name]
    end
  end
  [best, counts.values.first]
end

def row(impl, op, bytes, ns)
  format("  %-14s %-7s %12.1f %10.1f\n", impl, op, ns, bytes / ns * 1000)
end

def fail(label, what)
  puts "FAIL #{label}: #{what}"
  $failures += 1
end

# Live sink for the tree decode timer: every gate-checked field is folded in
# so the read-back cannot be skipped. Same field list the parity checks below
# assert (str, inner name/value, repeated lengths and last elements, the
# proto3 optionals, u64, color, plus every scalar the re-encode covers).
$sink = 0

def consume_tree(back, repeats)
  acc = back.i32 + back.i64 + back.u32 + back.u64 + back.s32 + back.s64 +
        back.fx32 + back.fx64 + back.sfx32 + back.sfx64 +
        (back.flag ? 1 : 0) + back.str.length + back.color +
        back.inner.name.length + back.inner.value +
        back.nums.length + back.tags.length + back.inners.length + back.flags.length +
        back.opt_i32 + back.opt_str.length +
        back.f32.to_s.length + back.f64.to_s.length +
        (repeats.zero? ? 0 : back.nums.last + back.inners.last.value +
         (back.flags.last ? 1 : 0))
  $sink = ($sink + acc) & 0xFFFFFFFFFFFFFFFF
  back
end

schema = Pbwire::Schema.from_file(File.expand_path('gen/matrix3.desc', __dir__))
shim_type = schema.type('structtest.Scalars3')

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
  model = fill_model(repeats)
  unless shim_type.encode_object(model) == reference
    fail(label, 'pbwire-ruby-shim and google-protobuf encode differently')
    next
  end
  # The tree arm is gated before it is timed: the same bytes through the
  # per-field full task, the same fields back, byte-exact round trip of the
  # materialized model. Mirrors pbench Payload.tree_encode/tree_decode.
  unless Pbwire::Gen::Structtest_Scalars3.encode(model) == reference
    fail(label, 'pbwire-ruby-tree and google-protobuf encode differently')
    next
  end
  tree_back = Pbwire::Gen::Structtest_Scalars3.decode(reference)
  tree_ok = (Pbwire::Gen::Structtest_Scalars3.encode(tree_back) == reference) &&
            tree_back.str == 'hello' &&
            tree_back.inner.name == 'kid' &&
            tree_back.inner.value == 42 &&
            tree_back.nums.length == repeats &&
            tree_back.tags.length == repeats &&
            tree_back.inners.length == repeats &&
            tree_back.flags.length == repeats &&
            (repeats.zero? || tree_back.inners[repeats - 1].value == repeats) &&
            tree_back.opt_i32 == -7 &&
            tree_back.opt_str == 'opt' &&
            tree_back.u64 == 18_446_744_073_709_551_615 &&
            tree_back.color == 2
  unless tree_ok
    fail(label, 'pbwire-ruby-tree decode differs')
    next
  end
  consume_tree(tree_back, repeats)
  shim_back = shim_type.decode_object(reference, MODEL_CLASSES)
  unless Pbwire::Gen::Structtest_Scalars3.encode(shim_back) == reference &&
         shim_back.inner.name == 'kid' &&
         shim_back.inner.value == 42 &&
         shim_back.nums.length == repeats &&
         shim_back.inners.length == repeats &&
         shim_back.opt_i32 == -7 &&
         shim_back.u64 == 18_446_744_073_709_551_615
    fail(label, 'pbwire-ruby-shim decode differs')
    next
  end
  decoder = schema.message('structtest.Scalars3')
  # The prebuilt handle behind the proxy row: decode must round-trip
  # byte-exactly (no materialization; the tree row covers the full task).
  back = decoder.decode(reference)
  back_ok = (back.encode == reference) &&
              back['str'] == 'hello' &&
              back['inner']['name'] == 'kid' &&
              back['inner']['value'] == 42 &&
              Array(back['nums']).length == repeats &&
              Array(back['inners']).length == repeats &&
              (repeats.zero? || back['inners'][repeats - 1]['value'] == repeats)
  back.close
  unless back_ok
    fail(label, 'pbwire-ruby decode differs')
    next
  end
  puts "OK   #{label} encoding parity (#{reference.bytesize} bytes)"

  # Headline encodes build a fresh model per iteration inside the timer
  # (message preparation included), on both sides. Proxy rows stay on
  # their prebuilt handles.
  # The count is calibrated on the prebuilt proxy op: every op calibrates
  # independently from the floor, and the proxy op stays first so the printed
  # count matches the old rows. Proxy rows are the old prebuilt task relabelled,
  # not re-measured as a new task; same-run timing only, never the headline.
  ns, count = measure(iterations,
                       'proxy_encode' => -> { message.encode },
                       'tree_encode' => -> { Pbwire::Gen::Structtest_Scalars3.encode(fill_model(repeats)) },
                       'shim_encode' => -> { shim_type.encode_object(fill_model(repeats)) },
                       'gem_encode' => -> { Structtest::Scalars3.encode(fill_official(repeats)) },
                       'proxy_decode' => -> { decoder.decode(reference).close },
                       'tree_decode' => -> { consume_tree(Pbwire::Gen::Structtest_Scalars3.decode(reference), repeats) },
                       'shim_decode' => -> { shim_type.decode_object(reference, MODEL_CLASSES) }, # eager: type_decode_object materializes every field in C before return (see header), so no folding sink is needed
                       'gem_decode' => -> { Structtest::Scalars3.decode(reference) })
  proxy_encode_ns = ns['proxy_encode']
  encode_ns = ns['tree_encode']
  shim_encode_ns = ns['shim_encode']
  proxy_decode_ns = ns['proxy_decode']
  decode_ns = ns['tree_decode']
  shim_decode_ns = ns['shim_decode']
  official_encode_ns = ns['gem_encode']
  official_decode_ns = ns['gem_decode']

  puts "#{label}: #{reference.bytesize} bytes, #{count} iterations " \
       "(repeated fields x#{repeats})"
  puts format("  %-14s %-7s %12s %10s", 'impl', 'op', 'ns/op', 'MB/s')
  puts row('pbwire-ruby-tree', 'encode', reference.bytesize, encode_ns)
  puts row('pbwire-ruby-shim', 'encode', reference.bytesize, shim_encode_ns)
  puts row('pbwire-ruby-proxy', 'encode', reference.bytesize, proxy_encode_ns)
  puts row('google-protobuf', 'encode', reference.bytesize, official_encode_ns)
  puts row('pbwire-ruby-tree', 'decode', reference.bytesize, decode_ns)
  puts row('pbwire-ruby-shim', 'decode', reference.bytesize, shim_decode_ns)
  puts row('pbwire-ruby-proxy', 'decode', reference.bytesize, proxy_decode_ns)
  puts row('google-protobuf', 'decode', reference.bytesize, official_decode_ns)
  puts
end

abort("#{$failures} check(s) failed") unless $failures.zero?
