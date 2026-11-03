# frozen_string_literal: true

# pbwire Ruby binding: a compiled extension over the C core, no gems.
#
# A FileDescriptorSet (`protoc --descriptor_set_out`) is the schema; messages
# are built and read by field name or number and go to/from wire bytes with no
# generated code. Every pbw_status is checked and turned into Pbwire::Error.
#
#   schema = Pbwire::Schema.from_file("sample.desc")
#   msg = schema.message("pbwire.ruby.Sample")
#   msg["name"] = "alice"
#   wire = msg.encode
#   schema.message("pbwire.ruby.Sample").decode(wire)["name"] # => "alice"
#
# Build it with build.sh (or `make ruby-build`); the extension is linked against
# the C core, so the schema and the wire format stay in one place.
require_relative 'pbwire_ext'
