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

# Idiomatic Ruby resource handling, mirroring File.open: with a block the
# schema closes itself (even on exception), without one the caller owns it
# and closes it explicitly. The C extension owns the format; this wrapper
# only owns the ergonomics, so no wire behavior changes here.
module Pbwire
  class Schema
    class << self
      alias_method :__pbwire_from_bytes__, :from_bytes
      alias_method :__pbwire_from_file__, :from_file

      def from_bytes(bytes)
        schema = __pbwire_from_bytes__(bytes)
        return schema unless block_given?

        begin
          yield schema
        ensure
          schema.close unless schema.closed?
        end
      end

      def from_file(path)
        schema = __pbwire_from_file__(path)
        return schema unless block_given?

        begin
          yield schema
        ensure
          schema.close unless schema.closed?
        end
      end
    end
  end
end
