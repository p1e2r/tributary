// SPDX-License-Identifier: MIT
// From-scratch TrueType parser, anti-aliased scanline rasterizer, SVG path
// rasterizer (for icons) and a glyph atlas.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gfx {

struct Vec2 { float x, y; };

// A path made of line segments (curves are flattened while building).
struct Path {
    std::vector<std::vector<Vec2>> contours;
    void move_to(float x, float y);
    void line_to(float x, float y);
    void quad_to(float cx, float cy, float x, float y);
    void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void close();
    Vec2 cur{0, 0}, start{0, 0};
};

// Coverage rasterizer (signed-area accumulation).
struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};
Bitmap rasterize(const Path& p, int w, int h, float scale, float dx, float dy, bool even_odd);

// SVG path "d" attribute parser (M L H V C S Q T A Z, absolute & relative).
Path parse_svg_path(std::string_view d);

class Font {
public:
    bool load(const uint8_t* data, size_t size);
    uint16_t glyph_index(uint32_t codepoint) const;
    float advance(uint16_t glyph) const;  // font units
    float kerning(uint16_t left, uint16_t right) const;  // font units (GPOS pair adjustments)
    Path outline(uint16_t glyph) const;   // font units, y up
    float units_per_em() const { return float(upem_); }
    float ascent() const { return float(ascent_); }
    float descent() const { return float(descent_); }  // negative
    float line_gap() const { return float(line_gap_); }

private:
    void outline_into(uint16_t glyph, Path& p, const float m[6], int depth) const;
    uint32_t table(const char* tag) const;
    void load_gpos_kerning();
    const uint8_t* d_ = nullptr;
    size_t n_ = 0;
    uint32_t cmap_ = 0, glyf_ = 0, loca_ = 0, hmtx_ = 0;
    int cmap_format_ = 0;
    int upem_ = 1000, ascent_ = 0, descent_ = 0, line_gap_ = 0, num_hmetrics_ = 0, num_glyphs_ = 0;
    bool long_loca_ = false;
    std::unordered_map<uint32_t, int16_t> kern_;  // (left<<16|right) -> adjust
};

enum class FontId : uint8_t { Regular = 0, Bold = 1, Mono = 2, Count = 3 };

struct GlyphInfo {
    // atlas placement (pixels)
    uint16_t ax = 0, ay = 0, aw = 0, ah = 0;
    int16_t off_x = 0, off_y = 0;  // bitmap top-left relative to pen (y down)
    bool valid = false;
};

class Atlas {
public:
    static constexpr int SIZE = 2048;
    Atlas() : px(size_t(SIZE) * SIZE, 0) {}
    // returns false when full
    bool alloc(int w, int h, uint16_t& x, uint16_t& y);
    void clear();
    void blit(const Bitmap& b, int x, int y);
    std::vector<uint8_t> px;
    int dirty_y0 = SIZE, dirty_y1 = 0;
    uint64_t generation = 0;  // bumps on clear()

private:
    int shelf_x_ = 1, shelf_y_ = 1, shelf_h_ = 0;
};

struct TextRun {
    struct G { uint16_t glyph; float x; FontId font; };
    std::vector<G> glyphs;
    float width = 0;
};

class TextSystem {
public:
    TextSystem();
    void set_scale(float s);  // pixels per logical unit
    float scale() const { return scale_; }

    // All sizes/positions in logical units.
    float measure(FontId f, float size, std::string_view text);
    TextRun shape(FontId f, float size, std::string_view text);
    // byte offset of the caret position closest to x
    size_t hit_test(FontId f, float size, std::string_view text, float x);
    // x position of byte offset
    float caret_x(FontId f, float size, std::string_view text, size_t byte_off);
    float line_height(FontId f, float size) const;
    float ascent(FontId f, float size) const;

    const GlyphInfo& glyph(FontId f, uint16_t g, float px_size, int subpx);
    const GlyphInfo& icon(const std::string& name, float px_size);
    bool has_icon(const std::string& name) const { return icon_paths_.count(name) > 0; }

    Atlas atlas;
    Font fonts[int(FontId::Count)];

private:
    float scale_ = 1;
    std::unordered_map<uint64_t, GlyphInfo> cache_;
    std::map<std::pair<std::string, int>, GlyphInfo> icon_cache_;
    std::map<std::string, std::pair<Path, float>> icon_paths_;
    void reset_cache();
};

// UTF-8 helpers
uint32_t utf8_next(std::string_view s, size_t& i);
size_t utf8_prev_boundary(std::string_view s, size_t i);
size_t utf8_next_boundary(std::string_view s, size_t i);
void utf8_append(std::string& s, uint32_t cp);

}  // namespace gfx
