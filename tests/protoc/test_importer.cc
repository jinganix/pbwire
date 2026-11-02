/* Self-check for the optional .proto importer: .proto -> FileDescriptorSet ->
 * pbwire schema, i.e. the two libraries composing through the standard format
 * and sharing no other type. */

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "pbwire-protoc/importer.h"
#include "pbwire.h"

#ifndef PBW_TEST_DATA_DIR
#error "PBW_TEST_DATA_DIR must point at tests/protoc/data"
#endif

static int failures;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                  \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static pbwp::Result
import_main(bool include_source_info)
{
    pbwp::Options options;

    options.import_paths.push_back(PBW_TEST_DATA_DIR);
    options.include_source_info = include_source_info;
    pbwp::Importer importer(std::move(options));
    return importer.import({"main.proto"});
}

static void
test_import(void)
{
    pbwp::Result result = import_main(false);
    google::protobuf::FileDescriptorSet files;

    CHECK(result.ok());
    CHECK(result.diagnostics().empty());
    CHECK(!result.value().empty());
    CHECK(files.ParseFromArray(result.value().data(),
                               static_cast<int>(result.value().size())));
    /* the whole import closure, once each */
    CHECK(files.file_size() == 2);
    {
        int deps = 0;
        int mains = 0;
        int i;

        for (i = 0; i < files.file_size(); i++) {
            const std::string &name = files.file(i).name();
            if (name == "dep.proto") {
                deps++;
            } else if (name == "main.proto") {
                mains++;
            }
        }
        CHECK(deps == 1 && mains == 1);
    }

    /* pbwire consumes the very same bytes */
    {
        pbw_schema *schema = NULL;
        const pbw_descriptor *shape;
        const pbw_descriptor *point;
        const pbw_field *field;
        pbw_message *msg;
        pbw_message *origin;
        uint8_t *buf = NULL;
        size_t len = 0;
        static const uint8_t expected[] = {0x0a, 0x04, 0x08, 0x01,
                                           0x10, 0x02, 0x18, 0x01};

        CHECK(pbw_descriptor_set_load(result.value().data(),
                                      result.value().size(),
                                      &schema) == PBW_OK);
        if (schema == NULL) {
            return;
        }
        shape = pbw_schema_find(schema, "pbwire.protoc.main.Shape");
        point = pbw_schema_find(schema, "pbwire.protoc.dep.Point");
        CHECK(shape != NULL && point != NULL);
        if (shape == NULL || point == NULL) {
            pbw_schema_free(schema);
            return;
        }
        field = pbw_descriptor_find_field_name(shape, "points");
        CHECK(field != NULL && pbw_field_label(field) == PBW_LABEL_REPEATED);
        CHECK(pbw_field_message_type(field) == point);
        field = pbw_descriptor_find_field_name(shape, "kind");
        CHECK(field != NULL && pbw_field_type(field) == PBW_TYPE_ENUM);
        field = pbw_descriptor_find_field_name(shape, "labels");
        CHECK(field != NULL && pbw_field_label(field) == PBW_LABEL_REPEATED);

        msg = pbw_message_new(shape);
        origin = pbw_message_set_message(msg, 1);
        CHECK(origin != NULL);
        CHECK(pbw_message_set(origin, 1, pbw_value_new_int(1)) == PBW_OK);
        CHECK(pbw_message_set(origin, 2, pbw_value_new_int(2)) == PBW_OK);
        CHECK(pbw_message_set(msg, 3, pbw_value_new_int(1)) == PBW_OK);
        CHECK(pbw_encode(msg, &buf, &len) == PBW_OK);
        CHECK(len == sizeof(expected) && std::memcmp(buf, expected, len) == 0);
        pbw_free(buf);
        pbw_message_free(msg);
        pbw_schema_free(schema);
    }
}

static void
test_source_info(void)
{
    pbwp::Result plain = import_main(false);
    pbwp::Result with_info = import_main(true);
    google::protobuf::FileDescriptorSet files;

    CHECK(plain.ok() && with_info.ok());
    CHECK(with_info.value().size() > plain.value().size());
    CHECK(files.ParseFromArray(with_info.value().data(),
                               static_cast<int>(with_info.value().size())));
    {
        int i;
        int with_locations = 0;

        for (i = 0; i < files.file_size(); i++) {
            if (files.file(i).source_code_info().location_size() > 0) {
                with_locations++;
            }
        }
        CHECK(with_locations == files.file_size());
    }
}

static void
test_errors(void)
{
    pbwp::Options options;
    pbwp::Importer importer(options);
    pbwp::Result result = importer.import({"missing.proto"});

    CHECK(!result.ok());
    CHECK(!result.diagnostics().empty());
    CHECK(result.value().empty());
    if (!result.diagnostics().empty()) {
        const pbwp::Diagnostic &diagnostic = result.diagnostics().front();
        CHECK(diagnostic.severity == pbwp::Severity::kError);
        CHECK(diagnostic.filename == "missing.proto");
        CHECK(diagnostic.ToString().find("error") != std::string::npos);
    }

    CHECK(!importer.import({}).ok());
}

static void
test_save(void)
{
    pbwp::Result result = import_main(false);
    const char *path = "test_importer_output.pb";
    std::string loaded;

    CHECK(result.ok());
    if (!result.ok()) {
        return;
    }
    CHECK(result.value().save(path));
    {
        std::ifstream in(path, std::ios::binary);
        loaded.assign(std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>());
    }
    CHECK(loaded.size() == result.value().size());
    CHECK(std::memcmp(loaded.data(), result.value().data(), loaded.size()) == 0);
    std::remove(path);
    CHECK(!result.value().save(""));
}

int
main(void)
{
    test_import();
    test_source_info();
    test_errors();
    test_save();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_importer: all checks passed\n");
    return 0;
}
