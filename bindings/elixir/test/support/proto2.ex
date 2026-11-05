defmodule Pbwire.Test.Addressbook.PhoneType do
  @moduledoc false
  use Protobuf, enum: true, syntax: :proto2

  field(:MOBILE, 0)
  field(:HOME, 1)
  field(:WORK, 2)
end

defmodule Pbwire.Test.Addressbook.PhoneNumber do
  @moduledoc false
  use Protobuf, syntax: :proto2

  field(:number, 1, optional: true, type: :string)
  field(:type, 2, optional: true, type: Pbwire.Test.Addressbook.PhoneType, enum: true)
end

defmodule Pbwire.Test.Addressbook.Person do
  @moduledoc false
  use Protobuf, syntax: :proto2

  field(:name, 1, required: true, type: :string)
  field(:id, 2, required: true, type: :int32)
  field(:email, 3, optional: true, type: :string)
  field(:phones, 4, repeated: true, type: Pbwire.Test.Addressbook.PhoneNumber)
  field(:nickname, 5, optional: true, type: :string, default: "anon")
end

defmodule Pbwire.Test.Scalars do
  @moduledoc false
  # proto2, one field per scalar type plus packed/unpacked repeated twins.
  use Protobuf, syntax: :proto2

  field(:f_int32, 1, optional: true, type: :int32)
  field(:f_int64, 2, optional: true, type: :int64)
  field(:f_uint32, 3, optional: true, type: :uint32)
  field(:f_uint64, 4, optional: true, type: :uint64)
  field(:f_sint32, 5, optional: true, type: :sint32)
  field(:f_sint64, 6, optional: true, type: :sint64)
  field(:f_fixed32, 7, optional: true, type: :fixed32)
  field(:f_fixed64, 8, optional: true, type: :fixed64)
  field(:f_sfixed32, 9, optional: true, type: :sfixed32)
  field(:f_sfixed64, 10, optional: true, type: :sfixed64)
  field(:f_bool, 11, optional: true, type: :bool)
  field(:f_float, 12, optional: true, type: :float)
  field(:f_double, 13, optional: true, type: :double)
  field(:f_string, 14, optional: true, type: :string)
  field(:f_bytes, 15, optional: true, type: :bytes)
  field(:r_packed, 16, repeated: true, type: :int32, packed: true)
  field(:r_unpacked, 17, repeated: true, type: :int32, packed: false)
end
