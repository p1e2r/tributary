// SPDX-License-Identifier: MIT
// Immediate-mode UI toolkit on top of gfx::Renderer.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gfx/renderer.hpp"
#include "../platform/platform.hpp"

namespace ui {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;
using Id = uint64_t;

struct Theme {
    bool dark = true;
    Color bg, bg_subtle, bg_inset, bg_toolbar, toolbar_hover, toolbar_active, toolbar_text, toolbar_text_dim;
    Color border, border_muted, text, text_muted, text_inverse, accent, accent_emphasis, accent_subtle;
    Color success, success_emphasis, danger, danger_emphasis, danger_subtle, attention, attention_subtle, done;
    Color row_hover, row_selected, row_selected_text, row_selected_focus;
    Color input_bg, input_border, focus_ring;
    Color btn_bg, btn_hover, btn_border, btn_text;
    Color diff_add_bg, diff_add_line, diff_del_bg, diff_del_line, diff_hunk_bg, diff_hunk_text, diff_gutter;
    Color diff_add_gutter_sel, diff_del_gutter_sel;
    Color overlay, shadow, tooltip_bg, tooltip_text, scrollbar;
    // GitHub Desktop specifics
    Color toolbar_border, border_contrast, modified, conflicted, badge_bg, badge_selected_bg;
    Color btn_primary, btn_primary_hover;
    Color diff_add_gutter, diff_del_gutter, diff_hunk_gutter, diff_hunk_border, diff_gutter_border, diff_line_number, diff_empty;
    static Theme light();
    static Theme dark_theme();
};

struct KeyPress {
    plat::Key key;
    uint8_t mods;
};

class Context {
public:
    Context(gfx::Renderer& r, plat::Window& w);

    // frame lifecycle
    void begin_frame();
    void end_frame();
    void feed(const plat::Event& e);

    gfx::Renderer& r;
    plat::Window& win;
    Theme theme;

    // ---------------- ids
    Id id(std::string_view s) const;
    Id id(std::string_view s, int64_t n) const;
    void push_id(std::string_view s);
    void push_id(int64_t n);
    void pop_id();

    // ---------------- input state
    float mx = -1000, my = -1000;
    bool mouse_down[3]{}, mouse_pressed[3]{}, mouse_released[3]{};
    int clicks = 0;           // click count of the latest press
    float scroll_x = 0, scroll_y = 0;
    uint8_t mods = 0;
    std::vector<KeyPress> keys;    // pressed this frame
    std::string text_input;   // typed this frame
    bool window_focused = true;

    bool key_pressed(plat::Key k, uint8_t mods_exact = 0xFF) const;
    // consume: removes the key so other widgets don't see it
    bool take_key(plat::Key k, uint8_t mods_exact = 0xFF);

    // ---------------- interaction
    // Is the mouse over `r` on the current layer, and not occluded by a higher layer?
    bool hovered(const Rect& rect) const;
    // Click behaviour: returns true when pressed and released over the rect.
    bool clicked(Id id, const Rect& rect, int button = 0);
    bool pressed(Id id, const Rect& rect, int button = 0);  // on mouse down
    bool is_active(Id id) const { return active_ == id; }
    void set_active(Id id) { active_ = id; }
    void clear_active() { active_ = 0; }
    Id active() const { return active_; }

    // keyboard focus
    Id focus = 0;
    void set_focus(Id id) { focus = id; focus_set_this_frame_ = true; }
    bool has_focus(Id id) const { return focus == id; }
    void register_focusable(Id id);  // for Tab ordering
    bool was_present(Id id) const;   // was this focusable drawn last frame?
    void capture_tab(Id id) { tab_capture_ = id; }  // this widget takes Tab itself (code editor)

    // layers & overlays
    int layer() const { return layer_; }
    void set_layer(int l) { layer_ = l; r.set_layer(l); }
    // Register an area that occludes everything below its layer (popups).
    void add_blocker(const Rect& rect, bool modal);
    bool top_layer_is_modal() const;
    int input_layer() const { return input_layer_prev_; }

    // cursor & redraw
    void set_cursor(plat::Cursor c) { cursor_ = c; }
    void request_redraw() { redraw_ = true; }
    bool wants_redraw() const { return redraw_; }
    void animate() { animating_ = true; }
    bool animating() const { return animating_; }
    double time = 0, dt = 0;

    // tooltips
    void tooltip(Id id, const Rect& anchor, const std::string& text);

    // per-widget persistent state
    struct TextState {
        size_t cursor = 0, anchor = 0;
        float scroll = 0, scroll_y = 0;
        double blink_t = 0;
        bool selecting = false;
        std::vector<std::string> undo;
    };
    TextState& text_state(Id id) { return text_states_[id]; }
    struct ScrollState {
        float offset = 0, target = -1, content = 0, view = 0;
        bool dragging = false;
        float drag_start_y = 0, drag_start_off = 0;
    };
    ScrollState& scroll_state(Id id) { return scroll_states_[id]; }

    // popups (open state keyed by id)
    bool popup_open(Id id) const {
        for (auto p : popups_) if (p == id) return true;
        return false;
    }
    int popup_depth(Id id) const {
        for (size_t i = 0; i < popups_.size(); i++) if (popups_[i] == id) return int(i);
        return 0;
    }
    bool popup_is_top(Id id) const { return !popups_.empty() && popups_.back() == id; }
    // Opening from inside a popup nests it; otherwise it replaces any open popup.
    void open_popup(Id id) {
        if (layer_ < 50) popups_.clear();
        if (!popup_open(id)) popups_.push_back(id);
        popup_opened_frame_ = frame_;
    }
    void close_popup() { if (!popups_.empty()) popups_.pop_back(); }
    void close_popup(Id id) {
        for (size_t i = 0; i < popups_.size(); i++)
            if (popups_[i] == id) { popups_.resize(i); return; }
    }
    void close_all_popups() { popups_.clear(); }
    Id open_popup_id() const { return popups_.empty() ? 0 : popups_.back(); }
    bool popup_just_opened() const { return popup_opened_frame_ == frame_ || popup_opened_frame_ + 1 == frame_; }

    uint64_t frame() const { return frame_; }

    // clipboard
    std::string clipboard() { return win.clipboard_get(); }
    void set_clipboard(const std::string& s) { win.clipboard_set(s); }

    // text helpers
    float text_w(FontId f, float size, std::string_view s) { return r.text().measure(f, size, s); }

private:
    Id tab_capture_ = 0, tab_capture_prev_ = 0;
    std::vector<Id> id_stack_;
    Id active_ = 0;
    int layer_ = 0;
    struct Blocker { Rect r; int layer; bool modal; };
    std::vector<Blocker> blockers_, blockers_prev_;
    int input_layer_prev_ = 0;
    plat::Cursor cursor_ = plat::Cursor::Arrow;
    bool redraw_ = false, animating_ = false;
    std::unordered_map<Id, TextState> text_states_;
    std::unordered_map<Id, ScrollState> scroll_states_;
    std::vector<Id> popups_;
    uint64_t popup_opened_frame_ = 0;
    uint64_t frame_ = 0;
    std::vector<Id> focusables_, focusables_prev_;
    bool focus_set_this_frame_ = false;
    Id tooltip_id_ = 0, tooltip_shown_ = 0;
    double tooltip_since_ = 0;
    Rect tooltip_anchor_{};
    std::string tooltip_text_;
    bool tooltip_requested_ = false;
    std::vector<std::pair<int, plat::Event>> pending_;
    friend struct Draw;
};

// ======================================================= widgets
enum class BtnStyle { Default, Primary, Danger, Invisible, Toolbar, Link };

bool button(Context& c, Id id, const Rect& r, const std::string& label, BtnStyle st = BtnStyle::Default,
            bool enabled = true, const char* icon = nullptr);
bool icon_button(Context& c, Id id, const Rect& r, const char* icon, const std::string& tip = "",
                 bool enabled = true, Color tint = {0, 0, 0, 0});
// tri-state: 0 off, 1 on, 2 mixed. Returns true when toggled.
bool checkbox(Context& c, Id id, const Rect& box, int state, bool enabled = true);
bool checkbox_label(Context& c, Id id, const Rect& r, bool& value, const std::string& label);
bool radio(Context& c, Id id, const Rect& r, bool selected, const std::string& label);
bool toggle(Context& c, Id id, const Rect& r, bool& value);

struct TextFieldOpts {
    std::string placeholder;
    bool password = false, mono = false, autofocus = false, read_only = false, multiline = false;
    float font_size = 12;
    bool borderless = false;  // the caller draws the box (commit description)
    const char* icon = nullptr;  // leading icon (e.g. search)
    bool invalid = false;
};
// Returns true when the text changed. `submitted` is set when Enter is pressed (single-line).
bool text_field(Context& c, Id id, const Rect& r, std::string& text, const TextFieldOpts& o = {}, bool* submitted = nullptr);

bool selectable(Context& c, Id id, const Rect& r, bool selected, bool* double_clicked = nullptr, bool* right_clicked = nullptr);
// Click handling for a list row without painting it (the caller paints).
bool row_click(Context& c, Id id, const Rect& r, bool* dbl = nullptr, bool* right = nullptr);

// Scroll views: begin returns the scroll offset; content drawn after this is
// clipped. Call end with the full content height.
float begin_scroll(Context& c, Id id, const Rect& r);
void end_scroll(Context& c, Id id, const Rect& r, float content_height);
void scroll_to_visible(Context& c, Id id, float item_y, float item_h, float view_h);

int tabs(Context& c, Id id, const Rect& r, const std::vector<std::string>& labels, int current,
         const std::vector<std::string>& badges = {});
void segmented(Context& c, Id id, const Rect& r, const std::vector<std::string>& labels, int& current);

// Popups
bool begin_popup(Context& c, Id id, const Rect& r);  // returns false if not open
bool begin_popup(Context& c, Id id, const Rect& r, Color bg, float radius);
void end_popup(Context& c);
bool menu_item(Context& c, Id id, const Rect& r, const std::string& label, const std::string& shortcut = "",
               bool enabled = true, const char* icon = nullptr, bool checked = false);
void separator(Context& c, const Rect& r);

// Modal dialog frame: returns the content rect. `closed` set when X / Escape.
Rect begin_modal(Context& c, Id id, const std::string& title, float w, float h, bool* closed);
void end_modal(Context& c);

void spinner(Context& c, float cx, float cy, float radius, Color col);
void progress_bar(Context& c, const Rect& r, float t, bool indeterminate = false);
void label(Context& c, const Rect& r, const std::string& text, FontId f = FontId::Regular, float size = 13,
           Color col = {0, 0, 0, 0}, Align a = Align::Left);
// Draws word-wrapped text; returns the height used.
float wrapped_text(Context& c, const Rect& r, const std::string& text, FontId f, float size, Color col,
                   float line_spacing = 1.35f, bool measure_only = false);
float splitter(Context& c, Id id, const Rect& r, float value, float min_v, float max_v);
void avatar(Context& c, const Rect& r, const std::string& name);
void badge(Context& c, float x, float y, const std::string& text, Color bg, Color fg, float* w_out = nullptr);

}  // namespace ui
