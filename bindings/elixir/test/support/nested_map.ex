defmodule Pbwire.Test.Nested.InnerEntry do
  @moduledoc false
  use Protobuf, map: true, syntax: :proto3

  field(:key, 1, type: :string)
  field(:value, 2, type: :int32)
end

defmodule Pbwire.Test.Nested.OuterEntry do
  @moduledoc false
  use Protobuf, map: true, syntax: :proto3

  field(:key, 1, type: :string)
  field(:value, 2, repeated: true, type: Pbwire.Test.Nested.InnerEntry, map: true)
end

defmodule Pbwire.Test.Nested.Doc do
  @moduledoc false
  # A map value that is itself a map: entries in entries on the wire.
  use Protobuf, syntax: :proto3

  field(:nested, 1, repeated: true, type: Pbwire.Test.Nested.OuterEntry, map: true)
end
