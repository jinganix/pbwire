/* .proto -> FileDescriptorSet, using the official Google Protobuf parser. */
#ifndef PBWIRE_PROTOC_IMPORTER_H
#define PBWIRE_PROTOC_IMPORTER_H

#include <string>
#include <vector>

#include "pbwire-protoc/descriptor_set.h"
#include "pbwire-protoc/error.h"

namespace pbwp {

struct Options {
    /* Search roots for import statements, like protoc -I. Empty means ".". */
    std::vector<std::string> import_paths;
    /* Keep SourceCodeInfo. pbwire never needs it; tools that map descriptors
     * back to .proto locations do. */
    bool include_source_info = false;
};

class Result {
  public:
    bool
    ok() const
    {
        return ok_;
    }
    /* Valid only when ok(); an empty set otherwise. */
    const DescriptorSet &
    value() const
    {
        return set_;
    }
    const std::vector<Diagnostic> &
    diagnostics() const
    {
        return diagnostics_;
    }

  private:
    friend class Importer;
    bool ok_ = false;
    DescriptorSet set_;
    std::vector<Diagnostic> diagnostics_;
};

/* Parses .proto files with the official Google Protobuf parser and exports the
 * full import closure as one FileDescriptorSet.
 *
 * One instance is one import context: not thread-safe, and a FileDescriptor
 * parsed by one instance is never handed to another. Create one per import. */
class Importer {
  public:
    explicit Importer(Options options);
    /* `files` are virtual paths relative to the import roots ("user.proto"). */
    Result import(const std::vector<std::string> &files);

  private:
    Options options_;
};

} /* namespace pbwp */

#endif /* PBWIRE_PROTOC_IMPORTER_H */
