# frozen_string_literal: true

# Interleaved timing for the Ruby arms: per round, `a-tree`, `b-shim` and the
# google-protobuf gem run back to back on the same payload; per tier the
# median of five rounds is printed. Bytes are compared before anything is
# timed (the gate does this too; this is the last guard before a number).
#
#   ruby bindings/ruby/pbench/compare.rb

require_relative 'payload'
require_relative 'a-tree'
require_relative 'b-shim'

ROUNDS = 5
PASS_SECONDS = 0.15

def ns_per_op(count)
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  count.times { yield }
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started) * 1e9 / count
end

def calibrate(op, floor)
  count = [1, floor].max
  loop do
    started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    count.times { op.call }
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    return count if elapsed >= PASS_SECONDS || count >= 100_000_000

    count *= 2
  end
end

def median(values)
  sorted = values.sort
  sorted[sorted.length / 2]
end

# Ops are [impl, op, callable]. Each gets one count, then every round runs
# every op once; the median per op wins.
def measure(ops, floor)
  counts = ops.map { |_, _, op| calibrate(op, floor) }
  counts.each_with_index { |count, i| ns_per_op(count, &ops[i][2]) }
  samples = ops.map { [] }
  ROUNDS.times do
    ops.each_with_index do |_, i|
      samples[i] << ns_per_op(counts[i], &ops[i][2])
    end
  end
  ops.each_with_index.map do |(impl, op, _), i|
    [impl, op, median(samples[i])]
  end
end

def row(impl, op, bytes, ns)
  format("  %-19s %-7s %12.1f %10.1f\n", impl, op, ns, bytes / ns * 1000)
end

unless Payload.type.respond_to?(:encode_object)
  puts 'FAIL shim arm missing: encode_object'
  exit 1
end

Payload::TIERS.each do |tier|
  label = tier[:label]
  repeats = tier[:repeats]
  bytes = tier[:bytes]
  ref = Payload.official_encode(repeats)
  if ref.bytesize != bytes
    puts "FAIL #{label}: official wrote #{ref.bytesize} bytes, expected #{bytes}"
    exit 1
  end
  obj = Payload.object(repeats)
  tree_wire = TreeArm.marshal(obj)
  shim_wire = ShimArm.marshal(obj)
  unless tree_wire == ref && shim_wire == ref
    puts "FAIL #{label}: tree/shim bytes differ from google-protobuf"
    exit 1
  end

  puts "#{label}: #{bytes} bytes, #{tier[:iterations]} iterations " \
       "(repeated fields x#{repeats})"
  puts format('  %-19s %-7s %12s %10s', 'impl', 'op', 'ns/op', 'MB/s')
  encode = measure([
                     ['pbwire-ruby-tree', 'encode', -> { TreeArm.marshal(obj) }],
                     ['pbwire-ruby-shim', 'encode', -> { ShimArm.marshal(obj) }],
                     ['google-protobuf', 'encode',
                      -> { Structtest::Scalars3.encode(Payload.official(repeats)) }]
                   ], tier[:iterations])
  encode.each { |impl, op, ns| puts row(impl, op, bytes, ns) }

  decode = measure([
                     ['pbwire-ruby-tree', 'decode', -> { TreeArm.unmarshal(ref) }],
                     ['pbwire-ruby-shim', 'decode', -> { ShimArm.unmarshal(ref) }],
                     ['google-protobuf', 'decode',
                      -> { Structtest::Scalars3.decode(ref) }]
                   ], tier[:iterations])
  decode.each { |impl, op, ns| puts row(impl, op, bytes, ns) }
  puts
end
