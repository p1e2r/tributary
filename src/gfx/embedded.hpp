// SPDX-License-Identifier: MIT
// Files compiled into the binary (fonts, SPIR-V shaders).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace embedded {
struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};
Blob get(std::string_view name);  // throws if missing
}  // namespace embedded
