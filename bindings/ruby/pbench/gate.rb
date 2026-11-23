# frozen_string_literal: true

# The pbench gate: no arm is timed before it reproduces the official
# google-protobuf bytes on all four tiers and reads its decode back field by
# field. Runs first and stays runnable:
#
#   ruby bindings/ruby/pbench/gate.rb
#
# Writes 'gate: PASS' and exits 0, or names the failing arm/tier and exits 1.

require_relative 'payload'

failures = 0

def fail(what)
  puts "FAIL #{what}"
end

def check(failures, ok, what)
  return failures if ok

  fail(what)
  failures + 1
end

type = Payload.type

Payload::TIERS.each do |tier|
  label = tier[:label]
  repeats = tier[:repeats]
  ref = Payload.official_encode(repeats)
  if ref.bytesize != tier[:bytes]
    failures = check(failures, false,
                     "#{label}: official wrote #{ref.bytesize} bytes, expected #{tier[:bytes]}")
    next
  end

  obj = Payload.object(repeats)
  tree_wire = Payload.tree_encode(obj)
  failures = check(failures, tree_wire == ref,
                   "#{label}: tree bytes differ from google-protobuf")
  tree_back = Payload.tree_decode(ref)
  failures = check(failures, Payload.reads_ok?(tree_back, repeats),
                   "#{label}: tree decode differs")
  failures = check(failures, Payload.tree_encode(tree_back) == ref,
                   "#{label}: tree re-encode differs")

  shim_wire = type.encode_object(obj)
  failures = check(failures, shim_wire == ref,
                   "#{label}: shim bytes differ from google-protobuf")
  shim_back = type.decode_object(ref, Payload::CLASSES)
  failures = check(failures, Payload.reads_ok?(shim_back, repeats),
                   "#{label}: shim decode differs")
  failures = check(failures, type.encode_object(shim_back) == ref,
                   "#{label}: shim re-encode differs")

  puts "OK   #{label}: #{ref.bytesize} bytes, tree/shim equal, x#{repeats}"
end

if failures.zero?
  puts 'gate: PASS'
else
  puts "#{failures} gate check(s) failed"
  exit 1
end
