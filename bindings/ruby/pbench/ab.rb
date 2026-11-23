# frozen_string_literal: true

# One shim cell for the Stage 6 A/B probe: format
#   ruby pbench/ab.rb <tier> <encode|decode>
# prints MB/s. ab.sh runs it against two extension builds, alternating rounds.

require_relative 'payload'
require_relative 'b-shim'

tier_name = ARGV[0] || 'xlarge'
op = ARGV[1] || 'encode'
tier = Payload::TIERS.find { |t| t[:label] == tier_name }
abort("unknown tier #{tier_name}") if tier.nil?
repeats = tier[:repeats]
bytes = tier[:bytes]
ref = Payload.official_encode(repeats)
obj = Payload.object(repeats)
unless ShimArm.marshal(obj) == ref
  abort('ab: shim bytes differ from google-protobuf')
end
if op == 'decode'
  back = ShimArm.unmarshal(ref)
  abort('ab: shim decode differs') unless Payload.reads_ok?(back, repeats)
end
count = tier[:iterations]
operation = op == 'decode' ? -> { ShimArm.unmarshal(ref) } : -> { ShimArm.marshal(obj) }

def pass(count, op)
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  count.times { op.call }
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started) * 1e9 / count
end

pass([count / 10, 1].max, operation)
samples = Array.new(5) { pass(count, operation) }
best = samples.min
puts format('%.1f', bytes / best * 1000)
