// The C example (examples/addressbook.c), in F#, through the generated
// records: assign fields, encode, decode, compare. The encoded bytes must
// equal the pinned reference examples/addressbook.bin.
//
//   dotnet run --project examples -- <addressbook.bin>

module Roundtrip

open System.IO

let phone number kind : Addressbook.Tutorial_Person_PhoneNumber =
    { number = number; ``type`` = Some kind }

let build () : Addressbook.Tutorial_AddressBook =
    let alice : Addressbook.Tutorial_Person =
        { name = "Alice"
          id = 1
          email = None
          phones = [ phone "555-0100" Addressbook.Tutorial_Person_PhoneType.MOBILE ] }
    let bob : Addressbook.Tutorial_Person =
        { name = "Bob"
          id = 2
          email = Some "bob@example.com"
          phones = [ phone "555-0200" Addressbook.Tutorial_Person_PhoneType.HOME ] }
    { people = [ alice; bob ] }

[<EntryPoint>]
let main argv =
    let bin =
        if argv.Length > 0 then argv[0]
        else "../../examples/addressbook.bin"

    let book = build ()
    let wire = Addressbook.Tutorial_AddressBook.encode book
    if File.Exists bin then
        let reference = File.ReadAllBytes bin
        if wire <> reference then
            eprintfn $"encode does not match %s{bin}"
            exit 1

    let back = Addressbook.Tutorial_AddressBook.decode wire
    printfn $"addressbook: %d{wire.Length} bytes, %d{back.people.Length} people"
    for p in back.people do
        let email = p.email |> Option.defaultValue ""
        printfn $"  %s{p.name} (%d{p.id}) %s{email}"
        for ph in p.phones do
            printfn $"    %s{ph.number}"
    0
