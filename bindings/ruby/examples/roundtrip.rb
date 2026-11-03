#!/usr/bin/env ruby
# frozen_string_literal: true

# Round trip without generated code: a FileDescriptorSet becomes the schema at
# runtime, a message is filled by field name, encoded, and decoded back.
#
#   make ruby-bind                                     # builds the extension
#   ruby -Ibindings/ruby/lib bindings/ruby/examples/roundtrip.rb [desc]
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'

desc = ARGV[0] || File.expand_path('../test/data/sample.desc', __dir__)
schema = Pbwire::Schema.from_file(desc)
sample = schema.message('pbwire.ruby.Sample')

sample['i32'] = -7
sample['name'] = 'alice'
sample['blob'] = "\x00\xff".b
sample['color'] = 1

inner = schema.message('pbwire.ruby.Inner')
inner['s'] = 'nested'
sample['inner'] = inner # moves the C message into the parent
sample['nums'] = [1, 2, 3] # repeated fields take the whole Array

wire = sample.encode
back = schema.message('pbwire.ruby.Sample').decode(wire)

raise "name lost: #{back['name'].inspect}" unless back['name'] == 'alice'
raise "nested message lost: #{back['inner'].inspect}" unless back['inner']['s'] == 'nested'
raise 're-encode differs from the wire bytes' unless back.encode == wire

puts "encoded #{wire.bytesize} bytes: #{wire.unpack1('H*')}"
puts "decoded name=#{back['name'].inspect} inner.s=#{back['inner']['s'].inspect} " \
     "nums=#{back['nums'].inspect} i32=#{back['i32']}"

back.close
sample.close
schema.close
