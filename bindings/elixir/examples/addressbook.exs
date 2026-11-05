# AddressBook example (Elixir): generated structs, then a plain round trip
# through Pbwire.
#
# Run from bindings/elixir (needs Elixir + OTP, `make setup` once):
#
#     mix run examples/addressbook.exs
#
# The modules below are what `protoc-gen-elixir` emits for an addressbook
# proto (see test/support/proto2.ex for the shared copy); a real project
# generates them from a `.proto` instead of writing them by hand.

defmodule Addressbook.PhoneType do
  use Protobuf, enum: true, syntax: :proto2
  field(:MOBILE, 0)
  field(:HOME, 1)
  field(:WORK, 2)
end

defmodule Addressbook.PhoneNumber do
  use Protobuf, syntax: :proto2
  field(:number, 1, optional: true, type: :string)
  field(:type, 2, optional: true, type: Addressbook.PhoneType, enum: true)
end

defmodule Addressbook.Person do
  use Protobuf, syntax: :proto2
  field(:name, 1, required: true, type: :string)
  field(:id, 2, required: true, type: :int32)
  field(:email, 3, optional: true, type: :string)
  field(:phones, 4, repeated: true, type: Addressbook.PhoneNumber)
end

defmodule Addressbook do
  alias Addressbook.{Person, PhoneNumber}

  def main do
    :ok = Pbwire.load_schema([Person])

    person = %Person{
      name: "alice",
      id: 1234,
      phones: [%PhoneNumber{number: "555-1234", type: :MOBILE}]
    }

    wire = Pbwire.encode(person)

    # Byte-identical to the official runtime on the same struct.
    true = wire == Protobuf.encode(person)

    ^person = Pbwire.decode(wire, Person)

    IO.puts(
      "#{byte_size(wire)} bytes, name=#{person.name}, id=#{person.id}, #{length(person.phones)} phone(s)"
    )
  end
end

Addressbook.main()
