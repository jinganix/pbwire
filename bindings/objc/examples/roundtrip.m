/* The C example (examples/addressbook.c), in Objective-C, through the generated
 * typed classes: assign fields, encode, decode, compare. The encoded bytes must
 * equal the pinned reference examples/addressbook.bin.
 *
 * Run it with ./build.sh example. */

#import <Foundation/Foundation.h>

#import "PBWMessage.h"
#import "PBWSchema.h"
#import "gen/addressbook.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                              \
        }                                                                            \
    } while (0)

static PBWGen_Person_PhoneNumber *
Phone(NSString *number, int32_t type)
{
    PBWGen_Person_PhoneNumber *phone = [PBWGen_Person_PhoneNumber new];
    phone.number = number;
    phone.hasType = YES;
    phone.type = type;
    return phone;
}

static PBWGen_AddressBook *
Build(void)
{
    PBWGen_Person *alice = [PBWGen_Person new];
    alice.name = @"Alice";
    alice.id = 1;
    alice.phones = @[ Phone(@"555-0100", PBWGen_Person_PhoneType_MOBILE) ];

    PBWGen_Person *bob = [PBWGen_Person new];
    bob.name = @"Bob";
    bob.id = 2;
    bob.hasEmail = YES;
    bob.email = @"bob@example.com";
    bob.phones = @[ Phone(@"555-0200", PBWGen_Person_PhoneType_HOME) ];

    PBWGen_AddressBook *book = [PBWGen_AddressBook new];
    book.people = @[ alice, bob ];
    return book;
}

int
main(int argc, char **argv)
{
    @autoreleasepool {
        NSString *desc = argc > 1 ? @(argv[1]) : @"examples/addressbook.desc";
        NSString *bin = argc > 2 ? @(argv[2]) : @"examples/addressbook.bin";
        NSError *error = nil;

        PBWSchema *schema = [PBWSchema schemaFromFile:desc error:&error];
        CHECK(schema != nil);

        NSData *wire = [Build() encodeWithSchema:schema error:&error];
        CHECK(wire != nil);
        NSData *reference = [NSData dataWithContentsOfFile:bin];
        CHECK([wire isEqualToData:reference]);

        PBWGen_AddressBook *back =
            [PBWGen_AddressBook decodeWithSchema:schema
                                            data:wire
                                           error:&error];
        CHECK(back.people.count == 2);
        printf("addressbook: %lu bytes, %lu people\n", (unsigned long)wire.length,
               (unsigned long)back.people.count);
        for (PBWGen_Person *person in back.people) {
            printf("  %s (%d) %s\n", person.name.UTF8String, person.id,
                   person.hasEmail ? person.email.UTF8String : "");
            for (PBWGen_Person_PhoneNumber *phone in person.phones) {
                printf("    %s\n", phone.number.UTF8String);
            }
        }
        return failures == 0 ? 0 : 1;
    }
}
