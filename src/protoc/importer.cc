#include "pbwire-protoc/importer.h"

#include <algorithm>
#include <set>
#include <utility>

#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"

namespace pbwp {
namespace {

using google::protobuf::FileDescriptor;
using google::protobuf::FileDescriptorProto;
using google::protobuf::FileDescriptorSet;

class Collector : public google::protobuf::compiler::MultiFileErrorCollector {
  public:
    explicit Collector(std::vector<Diagnostic> *out) : out_(out) {}

    void
    RecordError(absl::string_view filename, int line, int column,
                absl::string_view message) override
    {
        Add(Severity::kError, filename, line, column, message);
    }

    void
    RecordWarning(absl::string_view filename, int line, int column,
                  absl::string_view message) override
    {
        Add(Severity::kWarning, filename, line, column, message);
    }

  private:
    /* protobuf reports 0-based line/column, -1 for file-wide diagnostics */
    void
    Add(Severity severity, absl::string_view filename, int line,
        int column, absl::string_view message)
    {
        Diagnostic diagnostic;

        diagnostic.severity = severity;
        diagnostic.filename = std::string(filename);
        diagnostic.line = line < 0 ? 0 : line + 1;
        diagnostic.column = column < 0 ? 0 : column + 1;
        diagnostic.message = std::string(message);
        out_->push_back(std::move(diagnostic));
    }

    std::vector<Diagnostic> *out_;
};

/* Appends fd and its transitive imports, each file once, dependencies first:
 * a FileDescriptorSet is unordered but consumers resolve names as they go. */
void
AddFile(const FileDescriptor *fd, std::set<const FileDescriptor *> *seen,
        FileDescriptorSet *set, bool source_info)
{
    int i;

    if (!seen->insert(fd).second) {
        return;
    }
    for (i = 0; i < fd->dependency_count(); i++) {
        AddFile(fd->dependency(i), seen, set, source_info);
    }
    /* CopyTo() leaves SourceCodeInfo alone: it must be asked for separately */
    FileDescriptorProto *proto = set->add_file();
    fd->CopyTo(proto);
    if (source_info) {
        fd->CopySourceCodeInfoTo(proto);
    }
}

void
AddDiagnostic(std::vector<Diagnostic> *out, const std::string &filename,
              const std::string &message)
{
    Diagnostic diagnostic;

    diagnostic.filename = filename;
    diagnostic.message = message;
    out->push_back(std::move(diagnostic));
}

} /* namespace */

Importer::Importer(Options options) : options_(std::move(options)) {}

Result
Importer::import(const std::vector<std::string> &files)
{
    Result result;
    google::protobuf::compiler::DiskSourceTree source_tree;
    std::vector<const FileDescriptor *> roots;
    std::vector<std::string> paths = options_.import_paths;
    std::set<const FileDescriptor *> seen;
    FileDescriptorSet set;
    std::string bytes;
    size_t i;
    bool failed = false;

    if (files.empty()) {
        AddDiagnostic(&result.diagnostics_, "", "no input files");
        return result;
    }
    if (paths.empty()) {
        paths.push_back(".");
    }
    for (i = 0; i < paths.size(); i++) {
        source_tree.MapPath("", paths[i]);
    }
    Collector collector(&result.diagnostics_);
    google::protobuf::compiler::Importer importer(&source_tree, &collector);

    for (i = 0; i < files.size(); i++) {
        const FileDescriptor *fd = importer.Import(files[i]);

        if (fd == nullptr) {
            /* the collector usually reported why; make sure something did */
            if (result.diagnostics_.empty()) {
                AddDiagnostic(&result.diagnostics_, files[i], "import failed");
            }
            failed = true;
            continue;
        }
        roots.push_back(fd);
    }
    for (i = 0; i < result.diagnostics_.size(); i++) {
        if (result.diagnostics_[i].severity == Severity::kError) {
            failed = true;
        }
    }
    if (failed) {
        return result;
    }

    for (i = 0; i < roots.size(); i++) {
        AddFile(roots[i], &seen, &set, options_.include_source_info);
    }
    if (!set.SerializeToString(&bytes)) {
        AddDiagnostic(&result.diagnostics_, "", "failed to serialize FileDescriptorSet");
        return result;
    }
    result.set_ = DescriptorSet(std::move(bytes));
    result.ok_ = true;
    return result;
}

} /* namespace pbwp */
