# The addressbook example through the generated structs: assign fields,
# encode, compare with the pinned examples/addressbook.bin, decode it back.
#
#   julia --project=runtime examples/roundtrip_gen.jl [addressbook.bin]

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime", "src"))
using Pbwire
include(joinpath(@__DIR__, "gen", "addressbook_pbwire.jl"))

function phone(number::String, kind::Integer)
    return Tutorial_Person_PhoneNumber(number = number, type = Int32(kind))
end

function build()
    alice = Tutorial_Person(
        name = "Alice",
        id = Int32(1),
        phones = [phone("555-0100", Tutorial_Person_PhoneType.MOBILE)],
    )
    bob = Tutorial_Person(
        name = "Bob",
        id = Int32(2),
        email = "bob@example.com",
        phones = [phone("555-0200", Tutorial_Person_PhoneType.HOME)],
    )
    return Tutorial_AddressBook(people = [alice, bob])
end

function main(args::Vector{String})
    bin_path = isempty(args) ? joinpath(@__DIR__, "..", "..", "..", "examples", "addressbook.bin") : args[1]

    book = build()
    wire = encode(book)
    if isfile(bin_path)
        reference = read(bin_path)
        if wire != reference
            error("encode does not match $bin_path")
        end
    end

    back = decode(Tutorial_AddressBook, wire)
    println("addressbook: $(length(wire)) bytes, $(length(back.people)) people")
    for p in back.people
        print("  $(p.name) ($(p.id))")
        if p.email !== nothing
            print(" $(p.email)")
        end
        println()
        for ph in p.phones
            println("    $(ph.number)")
        end
    end
    return nothing
end

main(ARGS)
