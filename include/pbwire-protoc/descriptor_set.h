/* A serialized google.protobuf.FileDescriptorSet. */
#ifndef PBWIRE_PROTOC_DESCRIPTOR_SET_H
#define PBWIRE_PROTOC_DESCRIPTOR_SET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace pbwp {

/* Exactly what `protoc --descriptor_set_out` writes and what
 * pbw_descriptor_set_load() consumes: the two libraries meet here and share no
 * other type. */
class DescriptorSet {
  public:
    DescriptorSet() = default;
    explicit DescriptorSet(std::string bytes) : bytes_(std::move(bytes)) {}

    const uint8_t *
    data() const
    {
        return reinterpret_cast<const uint8_t *>(bytes_.data());
    }
    size_t
    size() const
    {
        return bytes_.size();
    }
    bool
    empty() const
    {
        return bytes_.empty();
    }

    /* Writes the set in binary mode; false on any I/O error. */
    bool save(const std::string &path) const;

  private:
    std::string bytes_;
};

} /* namespace pbwp */

#endif /* PBWIRE_PROTOC_DESCRIPTOR_SET_H */
