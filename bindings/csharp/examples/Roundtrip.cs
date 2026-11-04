// The C example (examples/addressbook.c), in C#, through the generated typed
// classes: assign typed fields, encode, decode, compare. The encoded bytes must
// equal the pinned reference examples/addressbook.bin.
//
//   dotnet run --project examples

using Pbwire;
using Pbwire.Generated;

var desc = args.Length > 0 ? args[0] : "examples/addressbook.desc";
var bin = args.Length > 1 ? args[1] : "examples/addressbook.bin";

using var schema = Schema.FromFile(desc);

var book = new Tutorial_AddressBook
{
    People =
    {
        new Tutorial_Person
        {
            Name = "Alice",
            Id = 1,
            Phones = { Phone("555-0100", Tutorial_Person_PhoneType.MOBILE) },
        },
        new Tutorial_Person
        {
            Name = "Bob",
            Id = 2,
            HasEmail = true,
            Email = "bob@example.com",
            Phones = { Phone("555-0200", Tutorial_Person_PhoneType.HOME) },
        },
    },
};

var wire = book.Encode(schema);
if (File.Exists(bin))
{
    var reference = File.ReadAllBytes(bin);
    if (!wire.AsSpan().SequenceEqual(reference))
    {
        throw new InvalidOperationException($"encode does not match {bin}");
    }
}

var back = Tutorial_AddressBook.Decode(schema, wire);
Console.WriteLine($"addressbook: {wire.Length} bytes, {back.People.Count} people");
foreach (var person in back.People)
{
    Console.WriteLine($"  {person.Name} ({person.Id}) {person.Email ?? ""}");
    foreach (var phone in person.Phones)
    {
        Console.WriteLine($"    {phone.Number}");
    }
}

static Tutorial_Person_PhoneNumber Phone(string number, int type) => new()
{
    Number = number,
    HasType = true,
    Type = type,
};
