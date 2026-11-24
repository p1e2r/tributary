// SPDX-License-Identifier: MIT
#include "ui.hpp"

#include <algorithm>
#include <cmath>

namespace ui {

using gfx::Color;

// Colors are GitHub Desktop's own (app/styles/_variables.scss and themes/_dark.scss,
// MIT licensed), with every Sass expression resolved.
Theme Theme::light() {
    Theme t;
    t.dark = false;
    auto H = [](uint32_t c, float a = 1.f) { return Color::hex(c, a); };
    t.bg = H(0xffffff); t.bg_subtle = H(0xf6f8fa); t.bg_inset = H(0xf6f8fa);
    t.bg_toolbar = H(0x24292e); t.toolbar_hover = H(0x2f363d); t.toolbar_active = H(0x586069);
    t.toolbar_text = H(0xffffff); t.toolbar_text_dim = H(0xd1d5da); t.toolbar_border = H(0x000000);
    t.border = H(0xe1e4e8); t.border_muted = H(0xe1e4e8); t.border_contrast = H(0x879099);
    t.text = H(0x24292e); t.text_muted = H(0x6a737d); t.text_inverse = H(0xffffff);
    t.accent = H(0x0372ef); t.accent_emphasis = H(0x0366d6); t.accent_subtle = H(0xf1f8ff);
    t.success = H(0x22863a); t.success_emphasis = H(0x28a745); t.danger = H(0xcb2431); t.danger_emphasis = H(0xd73a49);
    t.danger_subtle = H(0xffeef0); t.attention = H(0xb08800); t.attention_subtle = H(0xfffbdd); t.done = H(0x6f42c1);
    t.modified = H(0xaa8507); t.conflicted = H(0xc24e00);
    t.row_hover = H(0xf6f8fa); t.row_selected = H(0xebeef1); t.row_selected_text = H(0x24292e);
    t.row_selected_focus = H(0x0366d6);
    t.badge_bg = H(0xe1e4e8); t.badge_selected_bg = H(0xd1d5da);
    t.input_bg = H(0xffffff); t.input_border = H(0x879099); t.focus_ring = H(0x0366d6);
    t.btn_bg = H(0xf6f8fa); t.btn_hover = H(0xffffff); t.btn_border = H(0x879099); t.btn_text = H(0x24292e);
    t.btn_primary = H(0x0366d6); t.btn_primary_hover = H(0x0372ef);
    t.diff_add_bg = H(0xe6ffed); t.diff_add_line = H(0xacf2bd); t.diff_del_bg = H(0xffeef0); t.diff_del_line = H(0xfdb8c0);
    t.diff_add_gutter = H(0xcdffd8); t.diff_del_gutter = H(0xffdce0); t.diff_hunk_gutter = H(0xdbedff); t.diff_hunk_border = H(0xc8e1ff);
    t.diff_hunk_bg = H(0xf1f8ff); t.diff_hunk_text = H(0x586069); t.diff_gutter = H(0xffffff); t.diff_gutter_border = H(0xe1e4e8);
    t.diff_line_number = H(0x444d56); t.diff_empty = H(0xfafbfc);
    t.diff_add_gutter_sel = H(0x2188ff); t.diff_del_gutter_sel = H(0x2188ff);
    t.overlay = H(0x000000, 0.4f); t.shadow = Color{71 / 255.f, 83 / 255.f, 95 / 255.f, 0.19f}; t.tooltip_bg = H(0x24292e); t.tooltip_text = H(0xf6f8fa);
    t.scrollbar = H(0x000000, 0.2f);
    return t;
}

Theme Theme::dark_theme() {
    Theme t;
    t.dark = true;
    auto H = [](uint32_t c, float a = 1.f) { return Color::hex(c, a); };
    t.bg = H(0x24292e); t.bg_subtle = H(0x2b3137); t.bg_inset = H(0x1d2125);
    t.bg_toolbar = H(0x1d2125); t.toolbar_hover = H(0x2f363d); t.toolbar_active = H(0x444d56);
    t.toolbar_text = H(0xf6f8fa); t.toolbar_text_dim = H(0x959da5); t.toolbar_border = H(0x141414);
    t.border = H(0x141414); t.border_muted = H(0x141414); t.border_contrast = H(0x717b85);
    t.text = H(0xf6f8fa); t.text_muted = H(0x959da5); t.text_inverse = H(0xffffff);
    t.accent = H(0x2e8fff); t.accent_emphasis = H(0x0366d6); t.accent_subtle = H(0x0366d6, 0.25f);
    t.success = H(0x28a745); t.success_emphasis = H(0x28a745); t.danger = H(0xd73a49); t.danger_emphasis = H(0xd73a49);
    t.danger_subtle = H(0x450c0f); t.attention = H(0xf9c513); t.attention_subtle = H(0xf9c513, 0.15f); t.done = H(0x8a63d2);
    t.modified = H(0xdbab09); t.conflicted = H(0xf66a0a);
    t.row_hover = H(0x2f363d); t.row_selected = H(0x444d56); t.row_selected_text = H(0xf6f8fa);
    t.row_selected_focus = H(0x0366d6);
    t.badge_bg = H(0x586069); t.badge_selected_bg = H(0x6a737d);
    t.input_bg = H(0x1d2125); t.input_border = H(0x717b85); t.focus_ring = H(0x0366d6);
    t.btn_bg = H(0x2f363d); t.btn_hover = H(0x2f363d); t.btn_border = H(0x717b85); t.btn_text = H(0xf6f8fa);
    t.btn_primary = H(0x0366d6); t.btn_primary_hover = H(0x0372ef);
    t.diff_add_bg = H(0x113a1b); t.diff_add_line = H(0x22863a); t.diff_del_bg = H(0x450c0f); t.diff_del_line = H(0xb31d28);
    t.diff_add_gutter = H(0x0b2612); t.diff_del_gutter = H(0x30090a); t.diff_hunk_gutter = H(0x171a1d); t.diff_hunk_border = H(0x2b3137);
    t.diff_hunk_bg = H(0x1d2125); t.diff_hunk_text = H(0x959da5); t.diff_gutter = H(0x24292e); t.diff_gutter_border = H(0x2f363d);
    t.diff_line_number = H(0x959da5); t.diff_empty = H(0x22272b);
    t.diff_add_gutter_sel = H(0x044289); t.diff_del_gutter_sel = H(0x044289);
    t.overlay = H(0x000000, 0.5f); t.shadow = H(0x000000, 0.5f); t.tooltip_bg = H(0x2f363d); t.tooltip_text = H(0xf6f8fa);
    t.scrollbar = H(0xffffff, 0.2f);
    return t;
}

Context::Context(gfx::Renderer& rr, plat::Window& w) : r(rr), win(w), theme(Theme::dark_theme()) { id_stack_.push_back(0xcbf29ce484222325ull); }

static Id fnv(Id h, const void* d, size_t n) {
    auto p = static_cast<const uint8_t*>(d);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

Id Context::id(std::string_view s) const { return fnv(id_stack_.back(), s.data(), s.size()); }
Id Context::id(std::string_view s, int64_t n) const { Id h = id(s); return fnv(h, &n, sizeof n); }
void Context::push_id(std::string_view s) { id_stack_.push_back(id(s)); }
void Context::push_id(int64_t n) { id_stack_.push_back(fnv(id_stack_.back(), &n, sizeof n)); }
void Context::pop_id() { if (id_stack_.size() > 1) id_stack_.pop_back(); }

void Context::feed(const plat::Event& e) {
    using T = plat::Event;
    switch (e.type) {
        case T::MouseMove: mx = e.x; my = e.y; mods = e.mods; break;
        case T::MouseDown:
            mx = e.x; my = e.y;
            if (e.button < 3) {
                // a press and release within the same batch must still be seen as a click:
                // defer the release to the next frame if this frame already has a press
                mouse_down[e.button] = true;
                mouse_pressed[e.button] = true;
                clicks = e.clicks;
            }
            mods = e.mods;
            break;
        case T::MouseUp:
            mx = e.x; my = e.y;
            if (e.button < 3) {
                if (mouse_pressed[e.button]) pending_.push_back({0, e});
                else { mouse_down[e.button] = false; mouse_released[e.button] = true; }
            }
            break;
        case T::Scroll: scroll_x += e.dx; scroll_y += e.dy; mx = e.x; my = e.y; break;
        case T::KeyDown: keys.push_back({e.key, e.mods}); mods = e.mods; break;
        case T::KeyUp: mods = e.mods; break;
        case T::Text: text_input += e.text; break;
        case T::FocusGained: window_focused = true; break;
        case T::FocusLost: window_focused = false; for (auto& d : mouse_down) d = false; break;
        default: break;
    }
}

void Context::begin_frame() {
    frame_++;
    double now = plat::now_seconds();
    dt = time ? std::min(0.1, now - time) : 0.016;
    time = now;
    layer_ = 0;
    r.set_layer(0);
    cursor_ = plat::Cursor::Arrow;
    redraw_ = false;
    animating_ = false;
    blockers_prev_ = std::move(blockers_);
    blockers_.clear();
    input_layer_prev_ = 0;
    for (auto& b : blockers_prev_) input_layer_prev_ = std::max(input_layer_prev_, b.layer);
    focusables_prev_ = std::move(focusables_);
    focusables_.clear();
    focus_set_this_frame_ = false;
    tooltip_requested_ = false;
    tab_capture_prev_ = tab_capture_;
    tab_capture_ = 0;
    // Tab navigation between text fields
    for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i].key == plat::Key::Tab && !(keys[i].mods & (plat::ModCtrl | plat::ModSuper)) && !focusables_prev_.empty() &&
            !(focus && focus == tab_capture_prev_)) {
            auto it = std::find(focusables_prev_.begin(), focusables_prev_.end(), focus);
            int idx = it == focusables_prev_.end() ? -1 : int(it - focusables_prev_.begin());
            int n = int(focusables_prev_.size());
            idx = (keys[i].mods & plat::ModShift) ? (idx <= 0 ? n - 1 : idx - 1) : (idx + 1) % n;
            focus = focusables_prev_[size_t(idx)];
            auto& ts = text_states_[focus];
            ts.anchor = 0;
            ts.cursor = std::string::npos;  // select all on tab-in (resolved by the field)
            keys.erase(keys.begin() + long(i));
            break;
        }
    }
}

void Context::end_frame() {
    // tooltip
    if (tooltip_requested_) {
        if (tooltip_id_ != tooltip_shown_) { tooltip_shown_ = tooltip_id_; tooltip_since_ = time; }
        if (time - tooltip_since_ > 0.6) {
            set_layer(90);
            float w = text_w(FontId::Regular, 12, tooltip_text_) + 16;
            float x = std::clamp(tooltip_anchor_.x + tooltip_anchor_.w / 2 - w / 2, 4.f, r.width() - w - 4);
            float y = tooltip_anchor_.bottom() + 6;
            if (y + 26 > r.height()) y = tooltip_anchor_.y - 32;
            r.rect({x, y, w, 24}, theme.tooltip_bg, 6);
            r.draw_text_in(FontId::Regular, 12, {x + 8, y, w - 16, 24}, tooltip_text_, theme.tooltip_text);
            set_layer(0);
        } else {
            animate();
        }
    } else {
        tooltip_shown_ = 0;
    }
    win.set_cursor(cursor_);
    // reset per-frame input
    for (int i = 0; i < 3; i++) { mouse_pressed[i] = false; mouse_released[i] = false; }
    scroll_x = scroll_y = 0;
    keys.clear();
    text_input.clear();
    if (mouse_down[0] == false && active_) {
        // a widget that never saw the release shouldn't stay active
    }
    // apply deferred releases
    auto pend = std::move(pending_);
    pending_.clear();
    for (auto& [_, e] : pend) {
        mouse_down[e.button] = false;
        mouse_released[e.button] = true;
        redraw_ = true;
    }
    if (!focus_set_this_frame_ && focus && std::find(focusables_.begin(), focusables_.end(), focus) == focusables_.end()) {
        // focused widget disappeared
        focus = 0;
    }
}

bool Context::key_pressed(plat::Key k, uint8_t m) const {
    for (auto& kk : keys)
        if (kk.key == k && (m == 0xFF || kk.mods == m)) return true;
    return false;
}

bool Context::take_key(plat::Key k, uint8_t m) {
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i].key == k && (m == 0xFF || keys[i].mods == m)) {
            keys.erase(keys.begin() + long(i));
            return true;
        }
    return false;
}

void Context::add_blocker(const Rect& rect, bool modal) { blockers_.push_back({rect, layer_, modal}); }

bool Context::top_layer_is_modal() const {
    for (auto& b : blockers_prev_) if (b.modal && b.layer >= input_layer_prev_) return true;
    return false;
}

bool Context::hovered(const Rect& rect) const {
    if (!rect.contains(mx, my)) return false;
    if (!r.current_clip().contains(mx, my)) return false;
    for (auto& b : blockers_prev_) {
        if (b.layer <= layer_) continue;
        if (b.modal || b.r.contains(mx, my)) return false;
    }
    return true;
}

bool Context::pressed(Id id, const Rect& rect, int button) {
    if (mouse_pressed[button] && hovered(rect)) {
        active_ = id;
        return true;
    }
    return false;
}

bool Context::clicked(Id id, const Rect& rect, int button) {
    bool hov = hovered(rect);
    if (mouse_pressed[button] && hov) active_ = id;
    if (active_ == id && mouse_released[button]) {
        active_ = 0;
        return hov;
    }
    return false;
}

void Context::register_focusable(Id id) { focusables_.push_back(id); }
bool Context::was_present(Id id) const {
    return std::find(focusables_prev_.begin(), focusables_prev_.end(), id) != focusables_prev_.end();
}

void Context::tooltip(Id id, const Rect& anchor, const std::string& text) {
    if (text.empty() || mouse_down[0]) return;
    tooltip_requested_ = true;
    tooltip_id_ = id;
    tooltip_anchor_ = anchor;
    tooltip_text_ = text;
}

}  // namespace ui
