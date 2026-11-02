#include "pbwire-protoc/error.h"

#include <cstdio>

namespace pbwp {

std::string
Diagnostic::ToString() const
{
    std::string out = filename.empty() ? std::string("<unknown>") : filename;

    if (line > 0) {
        char where[32];
        std::snprintf(where, sizeof(where), ":%d:%d", line, column > 0 ? column : 1);
        out += where;
    }
    out += severity == Severity::kWarning ? ": warning: " : ": error: ";
    out += message;
    return out;
}

} /* namespace pbwp */
