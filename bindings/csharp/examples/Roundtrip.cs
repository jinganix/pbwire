// The C example (examples/addressbook.c), in C#, through the generated
// Google.Protobuf-shaped classes: assign typed fields, encode, decode,
// compare. The encoded bytes must equal the pinned reference
// examples/addressbook.bin.
//
//   dotnet run --project examples

using Tutorial;

var bin = Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "examples", "addressbook.bin");

var book = new AddressBook
{
    People =
    {
        new Person
        {
            Name = "Alice",
            Id = 1,
            Phones = { Phone("555-0100", Person.Types.PhoneType.Mobile) },
        },
        new Person
        {
            Name = "Bob",
            Id = 2,
            Email = "bob@example.com",
            Phones = { Phone("555-0200", Person.Types.PhoneType.Home) },
        },
    },
};

var wire = book.ToByteArray();
if (File.Exists(bin))
{
    var reference = File.ReadAllBytes(bin);
    if (!wire.AsSpan().SequenceEqual(reference))
    {
        throw new InvalidOperationException($"encode does not match {bin}");
    }
}

var back = AddressBook.Parser.ParseFrom(wire);
Console.WriteLine($"addressbook: {wire.Length} bytes, {back.People.Count} people");
foreach (var person in back.People)
{
    Console.WriteLine($"  {person.Name} ({person.Id}) {person.Email}");
    foreach (var phone in person.Phones)
    {
        Console.WriteLine($"    {phone.Number}");
    }
}

static Person.Types.PhoneNumber Phone(string number, Person.Types.PhoneType type) => new()
{
    Number = number,
    Type = type,
};
