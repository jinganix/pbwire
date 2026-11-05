# The addressbook example through the dynamic API: build a tutorial.AddressBook,
# encode it, compare with the pinned examples/addressbook.bin, decode it back.
#
#   julia --project=runtime examples/roundtrip.jl [addressbook.bin]

push!(LOAD_PATH, joinpath(@__DIR__, "..", "runtime"))
using Pbwire

function person(m::Message, name::String, id::Integer; email = nothing)
    set_string!(m, "name", name)
    set_int!(m, "id", id)
    if email !== nothing
        set_string!(m, "email", email)
    end
    return m
end

function phone(m::Message, number::String, type::Integer)
    set_string!(m, "number", number)
    set_int!(m, "type", type)
    return m
end

function main(args::Vector{String})
    bin_path = isempty(args) ? joinpath(@__DIR__, "..", "..", "..", "examples", "addressbook.bin") : args[1]
    desc_path = joinpath(dirname(bin_path), "addressbook.desc")
    schema = from_file(desc_path)
    book_ty = msgtype(schema, "tutorial.AddressBook")

    book = new_message(book_ty)
    alice = person(set_message!(book, "people"), "Alice", 1)
    phone(set_message!(alice, "phones"), "555-0100", 0) # MOBILE
    bob = person(set_message!(book, "people"), "Bob", 2; email = "bob@example.com")
    phone(set_message!(bob, "phones"), "555-0200", 1) # HOME

    wire = encode(book)
    if isfile(bin_path)
        reference = read(bin_path)
        if wire != reference
            error("encode does not match $bin_path")
        end
    end
    close(book)

    back = decode(book_ty, wire)
    people = Any[]
    for i in 1:Pbwire.count(back, "people")
        push!(people, to_dict(as_message(at(back, "people", i))))
    end
    println("addressbook: $(length(wire)) bytes, $(length(people)) people")
    for p in people
        print("  $(p["name"]) ($(p["id"]))")
        if haskey(p, "email")
            print(" $(p["email"])")
        end
        println()
        for ph in get(p, "phones", Any[])
            println("    $(ph["number"])")
        end
    end
    close(back)
    close(schema)
    return nothing
end

main(ARGS)
