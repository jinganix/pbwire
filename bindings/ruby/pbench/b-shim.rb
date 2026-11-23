# frozen_string_literal: true

# The `shim` arm: one C-extension entry per op. `encode_object` hands the
# generated model to the extension, which walks its instance variables in C
# and fills the tree; `decode_object` decodes and materializes the model
# classes in C. No per-field `set`/`get` round trip either way.

require_relative 'payload'

module ShimArm
  module_function

  def marshal(obj)
    Payload.type.encode_object(obj)
  end

  def unmarshal(wire)
    Payload.type.decode_object(wire, Payload::CLASSES)
  end
end
