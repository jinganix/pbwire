/* Shim: re-export the C core header as module CPbwire.
 *
 * The wire format lives in the C core; this binding owns no wire code. The
 * relative include keeps working wherever the checkout is, because this
 * package always sits at bindings/swift (five levels below the repo root). */
#include "../../../../../include/pbwire.h"
