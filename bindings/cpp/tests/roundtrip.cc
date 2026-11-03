/* C++ binding self-check: struct -> wire -> struct, plus the failure paths a
 * C++ caller hits. No framework, no dependencies beyond the C runtime.
 *
 *     make test-cpp
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gen/addressbook.h"
#include "pbwire.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                         __LINE__, #cond);                                \
            failures++;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_OK(expr) CHECK((expr) == PBW_OK)

static bool
person_equal(const tutorial_person_t *a, const tutorial_person_t *b)
{
    size_t i;

    if (std::strcmp(a->name, b->name) != 0 || a->id != b->id ||
        a->n_phones != b->n_phones) {
        return false;
    }
    for (i = 0; i < a->n_phones; i++) {
        if (std::strcmp(a->phones[i].number, b->phones[i].number) != 0 ||
            a->phones[i].has_type != b->phones[i].has_type) {
            return false;
        }
    }
    return true;
}

int
main(void)
{
    pbw_schema *schema = nullptr;
    tutorial_person_phone_number_t phones[2];
    tutorial_person_t in = {}, out = {};
    uint8_t *wire = nullptr;
    size_t wire_len = 0;

    CHECK_OK(pbw_struct_schema_load(ADDRESSBOOK_DESC_PATH, &schema));
    if (schema == nullptr) {
        return 1;
    }

    phones[0] = {};
    phones[0].number = "555-1234";
    phones[0].type = TUTORIAL_PERSON_PHONE_TYPE_MOBILE;
    phones[0].has_type = 1;
    phones[1] = {};
    phones[1].number = "555-9999";

    in.name = "alice";
    in.id = 1234;
    in.email = "alice@example.com";
    in.phones = phones;
    in.n_phones = 2;

    CHECK_OK(tutorial_person_encode(schema, &in, &wire, &wire_len));
    CHECK(wire != nullptr && wire_len > 0);
    memset(&out, 0, sizeof(out));
    CHECK_OK(tutorial_person_decode(schema, wire, wire_len, &out));
    CHECK(person_equal(&in, &out));
    tutorial_person_free(&out);
    pbw_free(wire);

    /* A missing required field must fail, not write a broken message. */
    in.name = nullptr;
    wire = nullptr;
    CHECK(pbw_struct_encode(schema, "tutorial.Person", tutorial_person_fields,
                            4, &in, &wire, &wire_len) == PBW_ERR_REQUIRED);
    CHECK(wire == nullptr);

    pbw_schema_free(schema);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
