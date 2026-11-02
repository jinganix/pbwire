#include "pbwire-protoc/descriptor_set.h"

#include <fstream>

namespace pbwp {

bool
DescriptorSet::save(const std::string &path) const
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    if (!out) {
        return false;
    }
    out.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    return out.good();
}

} /* namespace pbwp */
