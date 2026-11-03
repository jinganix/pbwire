/* AddressBook example (C++): plain structs -> wire bytes -> plain structs.
 *
 * Same round-trip as bindings/c/example/addressbook.c, written the C++ way:
 * aggregate initialization instead of designated initializers, nullptr
 * instead of NULL. The C runtime (../c/runtime/pbw_struct.{h,c}) and the
 * generated header (../c/example/gen/addressbook.h) are shared unchanged;
 * pbw_struct.h carries extern "C" guards so C++ can include it directly.
 *
 * The schema loads once from its FileDescriptorSet
 * (examples/addressbook.desc) and covers every message in the file, so
 * there is no per-message setup: assign structs, encode, decode, compare.
 * Silent on success (exit code only), like the C example.
 *
 * Build and run: `make examples` (from the repo root).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gen/addressbook.h"
#include "pbwire.h"

/* Field-by-field equality: decode must recover exactly what was encoded. */
static bool
person_equal(const tutorial_person_t *a, const tutorial_person_t *b)
{
    size_t i;

    if (std::strcmp(a->name, b->name) != 0 || a->id != b->id) {
        return false;
    }
    if ((a->email == nullptr) != (b->email == nullptr)) {
        return false;
    }
    if (a->email != nullptr && std::strcmp(a->email, b->email) != 0) {
        return false;
    }
    if (a->n_phones != b->n_phones) {
        return false;
    }
    for (i = 0; i < a->n_phones; i++) {
        if (std::strcmp(a->phones[i].number, b->phones[i].number) != 0 ||
            a->phones[i].type != b->phones[i].type ||
            a->phones[i].has_type != b->phones[i].has_type) {
            return false;
        }
    }
    return true;
}

static bool
book_equal(const tutorial_address_book_t *a, const tutorial_address_book_t *b)
{
    size_t i;

    if (a->n_people != b->n_people) {
        return false;
    }
    for (i = 0; i < a->n_people; i++) {
        if (!person_equal(&a->people[i], &b->people[i])) {
            return false;
        }
    }
    return true;
}

int
main()
{
    /* The whole demo input is aggregate initialization; encode borrows it. */
    tutorial_person_phone_number_t alice_phones[] = {
        {"555-0100", TUTORIAL_PERSON_PHONE_TYPE_MOBILE, 1},
    };
    tutorial_person_phone_number_t bob_phones[] = {
        {"555-0200", TUTORIAL_PERSON_PHONE_TYPE_HOME, 1},
    };
    tutorial_person_t people[] = {
        {"Alice", 1, nullptr, alice_phones, 1}, /* absent stays absent */
        {"Bob", 2, "bob@example.com", bob_phones, 1},
    };
    tutorial_address_book_t book = {people, 2};
    tutorial_address_book_t got;
    pbw_schema *schema = nullptr;
    uint8_t *wire = nullptr;
    size_t wire_len = 0;
    bool ok;

    /* One call loads every message in the .proto; no per-message setup. */
    if (pbw_struct_schema_load(ADDRESSBOOK_DESC_PATH, &schema) != PBW_OK) {
        std::fprintf(stderr, "load schema failed: %s\n", ADDRESSBOOK_DESC_PATH);
        return 1;
    }
    if (tutorial_address_book_encode(schema, &book, &wire, &wire_len) !=
        PBW_OK) {
        std::fprintf(stderr, "encode failed\n");
        pbw_schema_free(schema);
        return 1;
    }
    if (tutorial_address_book_decode(schema, wire, wire_len, &got) != PBW_OK) {
        std::fprintf(stderr, "decode failed\n");
        pbw_free(wire);
        pbw_schema_free(schema);
        return 1;
    }

    ok = book_equal(&book, &got);

    tutorial_address_book_free(&got);
    pbw_free(wire);
    pbw_schema_free(schema);
    return ok ? 0 : 1;
}
