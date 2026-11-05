/* Compile check that the re-exported header parses as C. The ABI itself
 * lives in libpbwire, which the final link picks up from ../../build. */
#include "CPbwire.h"

const char *
cpbwire_header_check(void)
{
    return "ok";
}
