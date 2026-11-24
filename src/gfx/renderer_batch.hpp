// SPDX-License-Identifier: MIT
// Shared between the Vulkan and Metal backends.
#pragma once
#include <vector>

#include "image.hpp"
#include "renderer.hpp"

namespace gfx {
struct Batch {
    int layer;
    Rect clip;
    uint64_t tex = 0;  // 0 = atlas only
    std::vector<Instance> items;
};

}  // namespace gfx
