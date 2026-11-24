// SPDX-License-Identifier: MIT
#include "embedded.hpp"

#include <stdexcept>
#include <string>

namespace embedded {
namespace {
struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    size_t size;
};
#include "embedded_data.inc"
}  // namespace

Blob get(std::string_view name) {
    for (auto& f : kEmbedded)
        if (name == f.name) return {f.data, f.size};
    throw std::runtime_error("missing embedded file " + std::string(name));
}
}  // namespace embedded
