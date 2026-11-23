# frozen_string_literal: true

# The `tree` arm: the generated model facade. Marshal builds a Message with
# one set per field and encodes it; Unmarshal decodes and reads one get per
# field back. What a user of the per-field model API pays for the whole task.
# No prebuilt handle before the timer: the model is the only input, the wire
# the only output, and Ruby's GC owns the model (no manual free exists on
# this path, same as the Python arm).

require_relative 'payload'

module TreeArm
  module_function

  def marshal(obj)
    Payload.tree_encode(obj)
  end

  def unmarshal(wire)
    Payload.tree_decode(wire)
  end
end
