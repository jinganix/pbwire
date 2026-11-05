-- The C example (examples/addressbook.c), in Lua, through the generated
-- typed tables: assign fields, encode, decode, compare. The schema travels
-- embedded in the generated module; no .desc file is read. The encoded
-- bytes must equal the pinned reference examples/addressbook.bin.
--
--   lua examples/roundtrip.lua   (from bindings/lua, after ./build.sh)

package.cpath = "./?.so;" .. package.cpath

local gen = dofile("examples/gen/addressbook.lua")

local Person = gen.Tutorial_Person
local PhoneNumber = gen.Tutorial_Person_PhoneNumber
local Book = gen.Tutorial_AddressBook
local PhoneType = gen.Tutorial_Person_PhoneType

local function phone(number, typ)
    return PhoneNumber.new({ number = number, has_type = true, type = typ })
end

local function build()
    return Book.new({ people = {
        Person.new({ name = "Alice", id = 1, phones = {
            phone("555-0100", PhoneType.MOBILE),
        } }),
        Person.new({ name = "Bob", id = 2, has_email = true,
                     email = "bob@example.com", phones = {
            phone("555-0200", PhoneType.HOME),
        } }),
    } })
end

local bin = arg and arg[1] or "../../examples/addressbook.bin"

local wire = Book.encode(build())

local f = io.open(bin, "rb")
if f ~= nil then
    local ref = f:read("*a")
    f:close()
    assert(wire == ref, "encode does not match " .. bin)
end

local back = Book.decode(wire)
print(string.format("addressbook: %d bytes, %d people", #wire, #back.people))
for _, person in ipairs(back.people) do
    print(string.format("  %s (%d) %s", person.name, person.id,
                        person.has_email and person.email or ""))
    for _, ph in ipairs(person.phones) do
        print("    " .. ph.number)
    end
end
