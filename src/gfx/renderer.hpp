// SPDX-License-Identifier: MIT
// Vulkan 2D renderer: batched instanced quads (rounded rects with borders,
// soft shadows, glyphs and icons from one atlas), scissor clipping.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "../platform/platform.hpp"
#include "font.hpp"
#include "image.hpp"

namespace gfx {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static constexpr Color hex(uint32_t rgb, float a = 1) {
        return {float((rgb >> 16) & 255) / 255.f, float((rgb >> 8) & 255) / 255.f, float(rgb & 255) / 255.f, a};
    }
    Color with_alpha(float na) const { return {r, g, b, na}; }
    Color mix(const Color& o, float t) const { return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t}; }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect intersect(const Rect& o) const;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Rect cut_left(float a) { Rect r{x, y, a, h}; x += a; w -= a; return r; }
    Rect cut_right(float a) { Rect r{x + w - a, y, a, h}; w -= a; return r; }
    Rect cut_top(float a) { Rect r{x, y, w, a}; y += a; h -= a; return r; }
    Rect cut_bottom(float a) { Rect r{x, y + h - a, w, a}; h -= a; return r; }
};

struct Instance {
    float rect[4];
    float uv[4];
    float color[4];
    float border_color[4];
    float params[4];  // radius, border, kind, softness
};

enum class Align { Left, Center, Right };

class Renderer {
public:
    explicit Renderer(plat::Window& win);
    ~Renderer();

    TextSystem& text() { return text_; }

    // Begin a frame. Returns false if the window is minimized (skip drawing).
    bool begin_frame(Color clear);
    void end_frame();

    // Logical-coordinate drawing API
    void rect(const Rect& r, Color c, float radius = 0, float border = 0, Color border_color = {});
    void shadow(const Rect& r, float radius, float blur, Color c);
    // Draws text with its top-left at (x, y); returns advance width.
    float draw_text(FontId f, float size, float x, float y, std::string_view s, Color c);
    // Draws text vertically centered in `r`, ellipsized to fit.
    float draw_text_in(FontId f, float size, const Rect& r, std::string_view s, Color c, Align a = Align::Left);
    void icon(const std::string& name, float x, float y, float size, Color c);
    // Draw an RGBA image, uploading it the first time `key` is seen (cached on the GPU,
    // evicted after a few seconds unused). `alpha` scales opacity.
    void image(uint64_t key, const Image& img, const Rect& r, float alpha = 1.f);
    void line_h(float x0, float x1, float y, Color c, float thickness = 1);
    void line_v(float x, float y0, float y1, Color c, float thickness = 1);

    void push_clip(const Rect& r);
    void pop_clip();
    Rect current_clip() const { return clips_.empty() ? Rect{0, 0, 1e6f, 1e6f} : clips_.back(); }

    // Layers: everything on a higher layer draws above lower ones.
    void set_layer(int layer);
    int layer() const { return layer_; }

    float scale() const { return scale_; }
    void set_zoom(float z) { zoom_ = z; }
    float zoom() const { return zoom_; }
    float width() const { return float(win_.pixel_width()) / scale_; }
    float height() const { return float(win_.pixel_height()) / scale_; }

    std::string ellipsize(FontId f, float size, std::string_view s, float max_w);
    std::string ellipsize_middle(FontId f, float size, std::string_view s, float max_w);

    // Read back the last presented frame as RGBA (for screenshots/tests).
    bool capture(std::vector<uint8_t>& rgba, int& w, int& h);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    plat::Window& win_;
    TextSystem text_;
    float scale_ = 1, zoom_ = 1;
    int layer_ = 0;
    std::vector<Rect> clips_;
    void push_instance(const Instance& in);
    void push_instance_tex(const Instance& in, uint64_t tex);
};

}  // namespace gfx
