// SPDX-License-Identifier: MIT
// From-scratch image decoders for image diffs: PNG (all color types, bit
// depths and Adam7), baseline JPEG and GIF (first frame).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gfx {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4
    std::string format;         // "PNG", "JPEG", "GIF"
    bool valid() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * size_t(h) * 4; }
};

// Throws std::runtime_error with a readable reason on unsupported/corrupt data.
Image decode_image(std::string_view data);
bool is_image_data(std::string_view data);
uint64_t hash_bytes(std::string_view data);

// |a-b| per channel; images of different sizes are compared top-left aligned.
Image difference_image(const Image& a, const Image& b);

}  // namespace gfx
