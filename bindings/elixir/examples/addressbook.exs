# AddressBook example (Elixir): generated modules, then a plain round trip
# through Pbwire.
#
# Run from bindings/elixir (needs Elixir + OTP, `make setup` once):
#
#     mix run examples/addressbook.exs
#
# The modules are what `protoc-gen-pbwire --pbwire_opt=lang=elixir` emits for
# addressbook.proto (see examples/gen/addressbook.ex); they stay exactly as
# the generator emits them, only the codec is swapped. struct/2 (instead of
# %Struct{} literals) keeps the script compilable before the required file
# below defines the modules.

Code.require_file("gen/addressbook.ex", __DIR__)

defmodule AddressbookExample do
  def main(bin_path) do
    mods = [Tutorial.Person, Tutorial.Person.PhoneNumber, Tutorial.AddressBook]
    :ok = Pbwire.load_schema(mods)

    book =
      struct(Tutorial.AddressBook,
        people: [
          struct(Tutorial.Person,
            name: "Alice",
            id: 1,
            phones: [
              struct(Tutorial.Person.PhoneNumber, number: "555-0100", type: :MOBILE)
            ]
          ),
          struct(Tutorial.Person,
            name: "Bob",
            id: 2,
            email: "bob@example.com",
            phones: [
              struct(Tutorial.Person.PhoneNumber, number: "555-0200", type: :HOME)
            ]
          )
        ]
      )

    wire = Pbwire.encode(book)

    if File.exists?(bin_path) do
      ^wire = File.read!(bin_path)
    end

    decoded = Pbwire.decode(wire, Tutorial.AddressBook)
    true = decoded == book

    IO.puts("#{byte_size(wire)} bytes, #{length(book.people)} people")

    Enum.each(book.people, fn person ->
      IO.puts("  #{person.name} (#{person.id}) #{person.email || ""}")

      Enum.each(person.phones, fn phone ->
        IO.puts("    #{phone.number}")
      end)
    end)
  end
end

[path | _] = System.argv() ++ ["../../examples/addressbook.bin"]
AddressbookExample.main(path)
