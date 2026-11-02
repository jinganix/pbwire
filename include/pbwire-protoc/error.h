/* Structured diagnostics from the .proto importer. */
#ifndef PBWIRE_PROTOC_ERROR_H
#define PBWIRE_PROTOC_ERROR_H

#include <string>

namespace pbwp {

enum class Severity { kError,
                      kWarning };

struct Diagnostic {
    Severity severity = Severity::kError;
    std::string filename;
    int line = 0;   /* 1-based; 0 when the diagnostic is file-wide */
    int column = 0; /* 1-based; 0 when unknown */
    std::string message;

    /* "file:line:col: message", for logs and CLI output. */
    std::string ToString() const;
};

} /* namespace pbwp */

#endif /* PBWIRE_PROTOC_ERROR_H */
