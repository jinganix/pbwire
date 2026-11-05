defmodule Pbwire.Test.Doc.AttrsEntry do
  @moduledoc false
  use Protobuf, map: true, syntax: :proto3

  field(:key, 1, type: :string)
  field(:value, 2, type: :int32)
end

defmodule Pbwire.Test.Doc.CountsEntry do
  @moduledoc false
  use Protobuf, map: true, syntax: :proto3

  field(:key, 1, type: :int32)
  field(:value, 2, type: Pbwire.Test.Doc)
end

defmodule Pbwire.Test.DocKind do
  @moduledoc false
  use Protobuf, enum: true, syntax: :proto3

  field(:UNKNOWN, 0)
  field(:A, 1)
  field(:B, 2)
end

defmodule Pbwire.Test.Doc do
  @moduledoc false
  # proto3: implicit presence, explicit optional, packed, maps, oneof, nesting.
  use Protobuf, syntax: :proto3

  field(:title, 1, type: :string)
  field(:count, 2, type: :int32)
  field(:flag, 3, type: :bool)
  field(:blob, 4, type: :bytes)
  field(:score, 5, type: :double)
  field(:ratio, 6, type: :float)
  field(:kind, 7, type: Pbwire.Test.DocKind, enum: true)
  field(:sub, 8, type: Pbwire.Test.Addressbook.Person)
  field(:labels, 9, repeated: true, type: :string)
  field(:nums, 10, repeated: true, type: :int32, packed: true)
  field(:attrs, 11, repeated: true, type: Pbwire.Test.Doc.AttrsEntry, map: true)
  field(:counts, 12, repeated: true, type: Pbwire.Test.Doc.CountsEntry, map: true)
  field(:comment, 13, proto3_optional: true, type: :string)
  field(:level, 14, proto3_optional: true, type: :int32)

  oneof(:choice, 0)
  field(:pick_a, 15, type: :string, oneof: 0)
  field(:pick_b, 16, type: :int32, oneof: 0)
  field(:pick_m, 17, type: Pbwire.Test.Addressbook.Person, oneof: 0)
end
