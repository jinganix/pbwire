# frozen_string_literal: true

# The C example (examples/addressbook.c), in Ruby, through the generated
# classes: assign fields, encode, decode, compare. The encoded bytes must equal
# the pinned reference examples/addressbook.bin. The schema travels embedded
# in the generated code; no .desc file is read.
#
#   ruby -Ibindings/ruby/lib bindings/ruby/examples/roundtrip.rb

$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'pbwire'
require_relative 'gen/addressbook'

def build
  alice = Pbwire::Gen::Tutorial_Person.new(
    name: 'Alice', id: 1,
    phones: [phone('555-0100', Pbwire::Gen::Tutorial_Person_PhoneType::MOBILE)]
  )
  bob = Pbwire::Gen::Tutorial_Person.new(
    name: 'Bob', id: 2, has_email: true, email: 'bob@example.com',
    phones: [phone('555-0200', Pbwire::Gen::Tutorial_Person_PhoneType::HOME)]
  )
  Pbwire::Gen::Tutorial_AddressBook.new(people: [alice, bob])
end

def phone(number, type)
  Pbwire::Gen::Tutorial_Person_PhoneNumber.new(number: number, has_type: true, type: type)
end

bin = ARGV[0] || File.expand_path('../../../examples/addressbook.bin', __dir__)

wire = build.to_proto
raise "encode does not match #{bin}" if File.exist?(bin) && wire != File.binread(bin)

back = Pbwire::Gen::Tutorial_AddressBook.decode(wire)
puts "addressbook: #{wire.bytesize} bytes, #{back.people.length} people"
back.people.each do |person|
  puts "  #{person.name} (#{person.id}) #{person.has_email ? person.email : ''}"
  person.phones.each { |phone| puts "    #{phone.number}" }
end
