// SPDX-License-Identifier: MIT
#include <cctype>
#include <algorithm>
#include <cmath>

#include "ui.hpp"

namespace ui {

using gfx::utf8_next_boundary;
using gfx::utf8_prev_boundary;
using plat::Key;

static constexpr float kRadius = 6;

// ================================================================ buttons
bool button(Context& c, Id id, const Rect& r, const std::string& label, BtnStyle st, bool enabled, const char* icon) {
    auto& T = c.theme;
    bool hov = enabled && c.hovered(r);
    bool click = enabled && c.clicked(id, r);
    bool down = c.is_active(id) && hov;
    if (hov) c.set_cursor(plat::Cursor::Hand);
    Color bg, fg, border{0, 0, 0, 0};
    switch (st) {
        case BtnStyle::Primary:
            bg = hov ? T.btn_primary_hover : T.btn_primary;
            fg = Color::hex(0xffffff);
            border = T.btn_primary;
            break;
        case BtnStyle::Danger:
            // destructive dialogs keep the primary look in Desktop, with the danger color
            bg = hov ? T.danger_emphasis.mix(Color::hex(0xffffff), 0.08f) : T.danger_emphasis;
            fg = Color::hex(0xffffff);
            border = T.danger_emphasis;
            break;
        case BtnStyle::Invisible:
            bg = hov ? T.row_hover : Color{0, 0, 0, 0};
            fg = T.accent;
            break;
        case BtnStyle::Link:
            bg = {0, 0, 0, 0};
            fg = T.accent;
            break;
        case BtnStyle::Toolbar:
            bg = down ? T.toolbar_active : hov ? T.toolbar_hover : Color{0, 0, 0, 0};
            fg = T.toolbar_text;
            break;
        default:
            bg = hov ? T.btn_hover : T.btn_bg;
            fg = T.btn_text;
            border = T.btn_border;
    }
    // [aria-disabled] { opacity: 0.6 }
    if (!enabled) { bg = bg.with_alpha(bg.a * 0.6f); fg = fg.with_alpha(0.6f); border = border.with_alpha(border.a * 0.6f); }
    if (st != BtnStyle::Link && st != BtnStyle::Toolbar) c.r.rect(r, bg, kRadius, border.a > 0 ? 1 : 0, border);
    else if (st == BtnStyle::Toolbar) c.r.rect(r, bg, 0);
    float tw = c.text_w(FontId::Regular, 12, label);
    float iw = icon ? 16 + (label.empty() ? 0 : 6) : 0;
    float x = r.x + (r.w - tw - iw) / 2;
    if (icon) {
        c.r.icon(icon, x, r.y + (r.h - 16) / 2, 16, fg);
        x += iw;
    }
    if (!label.empty()) {
        if (st == BtnStyle::Link && hov) c.r.line_h(x, x + tw, r.y + r.h / 2 + 7, fg);
        c.r.draw_text_in(FontId::Regular, 12, {x, r.y, r.right() - x - 4, r.h}, label, fg);
    }
    return click;
}

bool icon_button(Context& c, Id id, const Rect& r, const char* icon, const std::string& tip, bool enabled, Color tint) {
    bool hov = enabled && c.hovered(r);
    bool click = enabled && c.clicked(id, r);
    if (hov) {
        c.r.rect(r, c.theme.row_hover, kRadius);
        c.set_cursor(plat::Cursor::Hand);
        c.tooltip(id, r, tip);
    }
    Color col = tint.a > 0 ? tint : c.theme.text_muted;
    if (!enabled) col = col.with_alpha(0.4f);
    c.r.icon(icon, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, 16, col);
    return click;
}

bool checkbox(Context& c, Id id, const Rect& box, int state, bool enabled) {
    auto& T = c.theme;
    Rect hit = box.inset(-4);
    bool click = enabled && c.clicked(id, hit);
    bool hov = enabled && c.hovered(hit);
    Color bg = state ? T.accent_emphasis : T.input_bg;
    Color border = state ? T.accent_emphasis : (hov ? T.text_muted : T.border_contrast);
    if (!enabled) { bg = bg.with_alpha(0.5f); border = border.with_alpha(0.5f); }
    c.r.rect(box, bg, 3, 1, border);
    if (state == 1) c.r.icon("check", box.x + (box.w - 12) / 2, box.y + (box.h - 12) / 2, 12, Color::hex(0xffffff));
    else if (state == 2) c.r.rect({box.x + 3, box.y + box.h / 2 - 1, box.w - 6, 2}, Color::hex(0xffffff), 1);
    return click;
}

bool checkbox_label(Context& c, Id id, const Rect& r, bool& value, const std::string& text) {
    Rect box{r.x, r.y + (r.h - 16) / 2, 16, 16};
    bool click = c.clicked(id, r);
    checkbox(c, id ^ 1, box, value ? 1 : 0);
    c.r.draw_text_in(FontId::Regular, 12, {r.x + 22, r.y, r.w - 22, r.h}, text, c.theme.text);
    if (c.hovered(r)) c.set_cursor(plat::Cursor::Hand);
    if (click) value = !value;
    return click;
}

bool radio(Context& c, Id id, const Rect& r, bool selected, const std::string& text) {
    auto& T = c.theme;
    bool click = c.clicked(id, r);
    Rect b{r.x, r.y + (r.h - 16) / 2, 16, 16};
    c.r.rect(b, T.input_bg, 8, selected ? 4.5f : 1, selected ? T.accent_emphasis : T.border_contrast);
    c.r.draw_text_in(FontId::Regular, 12, {r.x + 22, r.y, r.w - 22, r.h}, text, T.text);
    if (c.hovered(r)) c.set_cursor(plat::Cursor::Hand);
    return click;
}

bool toggle(Context& c, Id id, const Rect& r, bool& value) {
    auto& T = c.theme;
    Rect t{r.x, r.y + (r.h - 20) / 2, 36, 20};
    bool click = c.clicked(id, r);
    if (click) value = !value;
    c.r.rect(t, value ? T.accent_emphasis : T.btn_bg, 10, 1, value ? T.accent_emphasis : T.border);
    float kx = value ? t.right() - 18 : t.x + 2;
    c.r.rect({kx, t.y + 2, 16, 16}, Color::hex(0xffffff), 8);
    if (c.hovered(r)) c.set_cursor(plat::Cursor::Hand);
    return click;
}

// ================================================================ text field
namespace {
bool is_word(unsigned char ch) { return std::isalnum(ch) || ch >= 0x80 || ch == '_'; }

size_t word_left(const std::string& s, size_t i) {
    while (i > 0 && !is_word((unsigned char)s[i - 1])) i--;
    while (i > 0 && is_word((unsigned char)s[i - 1])) i--;
    return i;
}
size_t word_right(const std::string& s, size_t i) {
    while (i < s.size() && !is_word((unsigned char)s[i])) i++;
    while (i < s.size() && is_word((unsigned char)s[i])) i++;
    return i;
}

struct Line { size_t start, end; };  // byte ranges (end excludes '\n')

std::vector<Line> wrap_lines(Context& c, FontId f, float size, const std::string& s, float width) {
    std::vector<Line> out;
    size_t pos = 0;
    while (true) {
        size_t nl = s.find('\n', pos);
        size_t para_end = nl == std::string::npos ? s.size() : nl;
        // soft-wrap paragraph
        size_t ls = pos;
        while (true) {
            std::string_view rest(s.data() + ls, para_end - ls);
            if (c.text_w(f, size, rest) <= width || rest.empty()) { out.push_back({ls, para_end}); break; }
            // find break
            size_t best = ls, i = ls;
            while (i < para_end) {
                size_t nx = utf8_next_boundary(s, i);
                if (c.text_w(f, size, std::string_view(s.data() + ls, nx - ls)) > width) break;
                i = nx;
                if (s[i - 1] == ' ') best = i;
            }
            if (best == ls) best = std::max(i, utf8_next_boundary(s, ls));
            out.push_back({ls, best});
            ls = best;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}
}  // namespace

bool text_field(Context& c, Id id, const Rect& r, std::string& text, const TextFieldOpts& o, bool* submitted) {
    auto& T = c.theme;
    auto& st = c.text_state(id);
    FontId font = o.mono ? FontId::Mono : FontId::Regular;
    float fs = o.font_size;
    bool changed = false;
    if (submitted) *submitted = false;
    c.register_focusable(id);
    if (o.autofocus && !c.was_present(id) && c.frame() > 1) {
        c.set_focus(id);
        st.cursor = text.size();
        st.anchor = 0;
    }
    bool hov = c.hovered(r);
    if (hov) c.set_cursor(plat::Cursor::IBeam);
    bool focused = c.has_focus(id);
    if (st.cursor == std::string::npos) {  // tabbed in: select all
        st.cursor = text.size();
        st.anchor = 0;
    }
    st.cursor = std::min(st.cursor, text.size());
    st.anchor = std::min(st.anchor, text.size());

    float pad_x = 5, pad_icon = o.icon ? 20 : 0;
    Rect inner{r.x + pad_x + pad_icon, r.y, r.w - 2 * pad_x - pad_icon, r.h};
    float lh = c.r.text().line_height(font, fs);
    std::string shown = o.password ? std::string(text.size(), '*') : text;

    std::vector<Line> lines;
    if (o.multiline) lines = wrap_lines(c, font, fs, shown, inner.w);
    auto line_of = [&](size_t off) -> size_t {
        for (size_t i = 0; i < lines.size(); i++)
            if (off <= lines[i].end && (i + 1 == lines.size() || off < lines[i + 1].start)) return i;
        return lines.empty() ? 0 : lines.size() - 1;
    };
    auto pos_to_offset = [&](float x, float y) -> size_t {
        if (!o.multiline) return c.r.text().hit_test(font, fs, shown, x - inner.x + st.scroll);
        float top = r.y + 5 - st.scroll_y;
        int li = std::clamp(int((y - top) / lh), 0, int(lines.size()) - 1);
        auto& L = lines[size_t(li)];
        return L.start + c.r.text().hit_test(font, fs, std::string_view(shown).substr(L.start, L.end - L.start), x - inner.x);
    };

    // mouse
    if (c.mouse_pressed[0] && hov) {
        c.set_focus(id);
        focused = true;
        c.set_active(id);
        size_t off = pos_to_offset(c.mx, c.my);
        if (c.clicks >= 3) { st.anchor = 0; st.cursor = text.size(); }
        else if (c.clicks == 2) { st.anchor = word_left(text, off); st.cursor = word_right(text, off); }
        else {
            st.cursor = off;
            if (!(c.mods & plat::ModShift)) st.anchor = off;
        }
        st.selecting = c.clicks == 1;
        st.blink_t = c.time;
    } else if (c.mouse_pressed[0] && focused && !hov) {
        // clicking elsewhere drops focus
        c.focus = 0;
        focused = false;
    }
    if (c.is_active(id) && c.mouse_down[0] && st.selecting) {
        st.cursor = pos_to_offset(c.mx, c.my);
        c.request_redraw();
    }
    if (c.is_active(id) && c.mouse_released[0]) { c.clear_active(); st.selecting = false; }

    // keyboard
    if (focused) {
        auto sel_lo = std::min(st.cursor, st.anchor), sel_hi = std::max(st.cursor, st.anchor);
        auto del_sel = [&]() {
            if (sel_lo == sel_hi) return false;
            text.erase(sel_lo, sel_hi - sel_lo);
            st.cursor = st.anchor = sel_lo;
            sel_hi = sel_lo;
            changed = true;
            return true;
        };
        auto push_undo = [&] {
            if (st.undo.empty() || st.undo.back() != text) st.undo.push_back(text);
            if (st.undo.size() > 100) st.undo.erase(st.undo.begin());
        };
        if (!o.read_only && !c.text_input.empty()) {
            push_undo();
            del_sel();
            std::string in = c.text_input;
            if (!o.multiline) in.erase(std::remove(in.begin(), in.end(), '\n'), in.end());
            text.insert(st.cursor, in);
            st.cursor += in.size();
            st.anchor = st.cursor;
            changed = true;
            c.text_input.clear();
        }
        auto keys = c.keys;
        for (auto& k : keys) {
            bool shift = k.mods & plat::ModShift;
            bool cmd = k.mods & plat::ModCmd;
            bool word = k.mods & (
#ifdef __APPLE__
                plat::ModAlt
#else
                plat::ModCtrl
#endif
            );
            auto move = [&](size_t to) {
                st.cursor = to;
                if (!shift) st.anchor = to;
            };
            bool handled = true;
            switch (k.key) {
                case Key::Left:
                    if (!shift && sel_lo != sel_hi) move(sel_lo);
                    else move(cmd && !word ? (o.multiline ? lines[line_of(st.cursor)].start : 0)
                              : word ? word_left(text, st.cursor) : utf8_prev_boundary(text, st.cursor));
                    break;
                case Key::Right:
                    if (!shift && sel_lo != sel_hi) move(sel_hi);
                    else move(cmd && !word ? (o.multiline ? lines[line_of(st.cursor)].end : text.size())
                              : word ? word_right(text, st.cursor) : utf8_next_boundary(text, st.cursor));
                    break;
                case Key::Home: move(o.multiline && !cmd ? lines[line_of(st.cursor)].start : 0); break;
                case Key::End: move(o.multiline && !cmd ? lines[line_of(st.cursor)].end : text.size()); break;
                case Key::Up:
                case Key::Down: {
                    if (!o.multiline) { handled = false; break; }
                    size_t li = line_of(st.cursor);
                    float x = c.r.text().caret_x(font, fs, std::string_view(shown).substr(lines[li].start, lines[li].end - lines[li].start),
                                                 st.cursor - lines[li].start);
                    if (k.key == Key::Up && li == 0) { move(0); break; }
                    if (k.key == Key::Down && li + 1 >= lines.size()) { move(text.size()); break; }
                    auto& L = lines[k.key == Key::Up ? li - 1 : li + 1];
                    move(L.start + c.r.text().hit_test(font, fs, std::string_view(shown).substr(L.start, L.end - L.start), x));
                    break;
                }
                case Key::Backspace:
                    if (o.read_only) break;
                    push_undo();
                    if (!del_sel() && st.cursor > 0) {
                        size_t from = word ? word_left(text, st.cursor) : utf8_prev_boundary(text, st.cursor);
                        if (cmd && !word) from = 0;
                        text.erase(from, st.cursor - from);
                        st.cursor = st.anchor = from;
                        changed = true;
                    }
                    break;
                case Key::Delete:
                    if (o.read_only) break;
                    push_undo();
                    if (!del_sel() && st.cursor < text.size()) {
                        size_t to = word ? word_right(text, st.cursor) : utf8_next_boundary(text, st.cursor);
                        text.erase(st.cursor, to - st.cursor);
                        changed = true;
                    }
                    break;
                case Key::A: if (cmd) { st.anchor = 0; st.cursor = text.size(); } else handled = false; break;
                case Key::C:
                case Key::X:
                    if (!cmd) { handled = false; break; }
                    if (sel_lo != sel_hi && !o.password) c.set_clipboard(text.substr(sel_lo, sel_hi - sel_lo));
                    if (k.key == Key::X && !o.read_only) { push_undo(); del_sel(); }
                    break;
                case Key::V:
                    if (!cmd || o.read_only) { handled = !cmd ? false : true; break; }
                    {
                        push_undo();
                        std::string clip = c.clipboard();
                        clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
                        if (!o.multiline) std::replace(clip.begin(), clip.end(), '\n', ' ');
                        del_sel();
                        text.insert(st.cursor, clip);
                        st.cursor += clip.size();
                        st.anchor = st.cursor;
                        changed = true;
                    }
                    break;
                case Key::Z:
                    if (!cmd) { handled = false; break; }
                    if (!st.undo.empty()) {
                        text = st.undo.back();
                        st.undo.pop_back();
                        st.cursor = st.anchor = text.size();
                        changed = true;
                    }
                    break;
                case Key::Enter:
                    if (o.multiline && !cmd && !o.read_only) {
                        push_undo();
                        del_sel();
                        text.insert(st.cursor, "\n");
                        st.cursor = st.anchor = st.cursor + 1;
                        changed = true;
                    } else {
                        handled = false;
                        if (!o.multiline && submitted) { *submitted = true; handled = true; }
                    }
                    break;
                case Key::Escape: handled = false; break;
                default: handled = false;
            }
            if (handled) {
                c.take_key(k.key, k.mods);
                st.blink_t = c.time;
                sel_lo = std::min(st.cursor, st.anchor);
                sel_hi = std::max(st.cursor, st.anchor);
            }
        }
        if (changed) {
            shown = o.password ? std::string(text.size(), '*') : text;
            if (o.multiline) lines = wrap_lines(c, font, fs, shown, inner.w);
        }
    }

    // draw
    Color border = o.invalid ? T.danger : focused ? T.focus_ring : T.input_border;
    if (!o.borderless) {
        if (focused) c.r.rect(r.inset(-1), Color{3 / 255.f, 102 / 255.f, 214 / 255.f, 0.25f}, kRadius + 1);
        c.r.rect(r, o.read_only ? T.bg_subtle : T.input_bg, kRadius, 1, border);
    }
    if (o.icon) c.r.icon(o.icon, r.x + 5, r.y + (std::min(r.h, 25.f) - 16) / 2, 16, T.text_muted);
    c.r.push_clip(inner.inset(-1, 1));
    auto sel_lo = std::min(st.cursor, st.anchor), sel_hi = std::max(st.cursor, st.anchor);
    Color selc = T.accent_emphasis.with_alpha(focused ? 0.35f : 0.2f);
    bool blink_on = std::fmod(c.time - st.blink_t, 1.0) < 0.55;
    if (!o.multiline) {
        float cx = c.r.text().caret_x(font, fs, shown, st.cursor);
        if (cx - st.scroll > inner.w - 2) st.scroll = cx - inner.w + 2;
        if (cx - st.scroll < 0) st.scroll = cx;
        if (c.r.text().measure(font, fs, shown) < inner.w) st.scroll = 0;
        float tx = inner.x - st.scroll;
        float ty = r.y + (r.h - lh) / 2;
        if (sel_lo != sel_hi && focused) {
            float a = c.r.text().caret_x(font, fs, shown, sel_lo), b = c.r.text().caret_x(font, fs, shown, sel_hi);
            c.r.rect({tx + a, ty, b - a, lh}, selc);
        }
        if (text.empty() && !o.placeholder.empty())
            c.r.draw_text(font, fs, inner.x, ty, o.placeholder, T.text_muted);
        else
            c.r.draw_text(font, fs, tx, ty, shown, T.text);
        if (focused && blink_on && !o.read_only) c.r.rect({std::round(tx + cx), ty + 1, 1.5f, lh - 2}, T.text);
        if (focused) c.win.set_text_input_rect(tx + cx, ty, 1, lh);
    } else {
        float top = r.y + 5;
        size_t cli = line_of(st.cursor);
        float cy = float(cli) * lh;
        if (cy - st.scroll_y > r.h - 12 - lh) st.scroll_y = cy - (r.h - 12 - lh);
        if (cy - st.scroll_y < 0) st.scroll_y = cy;
        float total = float(lines.size()) * lh;
        if (hov && c.scroll_y != 0 && total > r.h - 12) {
            st.scroll_y = std::clamp(st.scroll_y - c.scroll_y, 0.f, total - (r.h - 12));
            c.scroll_y = 0;
        }
        if (text.empty() && !o.placeholder.empty())
            c.r.draw_text(font, fs, inner.x, top, o.placeholder, T.text_muted);
        for (size_t i = 0; i < lines.size(); i++) {
            float y = top + float(i) * lh - st.scroll_y;
            if (y + lh < r.y || y > r.bottom()) continue;
            auto& L = lines[i];
            std::string_view seg(shown.data() + L.start, L.end - L.start);
            if (focused && sel_lo != sel_hi && sel_hi >= L.start && sel_lo <= L.end) {
                size_t a = std::max(sel_lo, L.start) - L.start, b = std::min(sel_hi, L.end) - L.start;
                float ax = c.r.text().caret_x(font, fs, seg, a), bx = c.r.text().caret_x(font, fs, seg, b);
                if (sel_hi > L.end && i + 1 < lines.size()) bx += 4;
                c.r.rect({inner.x + ax, y, bx - ax, lh}, selc);
            }
            c.r.draw_text(font, fs, inner.x, y, seg, T.text);
            if (focused && i == cli && blink_on && !o.read_only) {
                float cx = c.r.text().caret_x(font, fs, seg, st.cursor - L.start);
                c.r.rect({std::round(inner.x + cx), y + 1, 1.5f, lh - 2}, T.text);
            }
        }
    }
    c.r.pop_clip();
    if (focused) c.animate();
    return changed;
}

// ================================================================ lists
bool row_click(Context& c, Id id, const Rect& r, bool* dbl, bool* right) {
    bool hov = c.hovered(r);
    bool click = c.clicked(id, r);
    if (dbl) *dbl = false;
    if (right) *right = false;
    if (click && dbl && c.clicks >= 2) *dbl = true;
    if (right && hov && c.mouse_pressed[1]) *right = true;
    return click;
}

bool selectable(Context& c, Id id, const Rect& r, bool selected, bool* dbl, bool* right) {
    bool hov = c.hovered(r);
    bool click = c.clicked(id, r);
    if (dbl) *dbl = false;
    if (right) *right = false;
    if (click && dbl && c.clicks >= 2) *dbl = true;
    if (right && hov && c.mouse_pressed[1]) *right = true;
    if (selected) c.r.rect(r, c.theme.row_selected);
    else if (hov) c.r.rect(r, c.theme.row_hover);
    return click;
}

float begin_scroll(Context& c, Id id, const Rect& r) {
    auto& s = c.scroll_state(id);
    s.view = r.h;
    if (c.hovered(r) && c.scroll_y != 0) {
        float max_off = std::max(0.f, s.content - r.h);
        s.target = std::clamp((s.target >= 0 ? s.target : s.offset) - c.scroll_y, 0.f, max_off);
        c.scroll_y = 0;
    }
    if (s.target >= 0) {
        float d = s.target - s.offset;
        if (std::fabs(d) < 0.5f) { s.offset = s.target; s.target = -1; }
        else { s.offset += d * std::min(1.0f, float(c.dt) * 22.f); c.animate(); }
    }
    s.offset = std::clamp(s.offset, 0.f, std::max(0.f, s.content - r.h));
    c.r.push_clip(r);
    return std::round(s.offset);
}

void end_scroll(Context& c, Id id, const Rect& r, float content) {
    auto& s = c.scroll_state(id);
    s.content = content;
    c.r.pop_clip();
    if (content <= r.h + 0.5f) { s.offset = 0; return; }
    float track_h = r.h - 4;
    float thumb_h = std::max(24.f, track_h * r.h / content);
    float max_off = content - r.h;
    float thumb_y = r.y + 2 + (track_h - thumb_h) * (s.offset / max_off);
    Rect thumb{r.right() - 8, thumb_y, 6, thumb_h};
    Rect track{r.right() - 10, r.y, 10, r.h};
    Id tid = id ^ 0x5c011;
    bool hov = c.hovered(track);
    if (c.pressed(tid, track)) {
        s.dragging = true;
        s.drag_start_y = c.my;
        if (!thumb.contains(c.mx, c.my)) {
            s.offset = std::clamp((c.my - r.y - thumb_h / 2) / (track_h - thumb_h) * max_off, 0.f, max_off);
        }
        s.drag_start_off = s.offset;
    }
    if (s.dragging) {
        if (c.mouse_down[0]) {
            s.offset = std::clamp(s.drag_start_off + (c.my - s.drag_start_y) / (track_h - thumb_h) * max_off, 0.f, max_off);
            s.target = -1;
            c.request_redraw();
        } else {
            s.dragging = false;
            c.clear_active();
        }
    }
    c.r.rect(thumb, c.theme.scrollbar.with_alpha(hov || s.dragging ? 0.8f : c.theme.scrollbar.a), 3);
}

void scroll_to_visible(Context& c, Id id, float y, float h, float view_h) {
    auto& s = c.scroll_state(id);
    if (y < s.offset) s.offset = y;
    else if (y + h > s.offset + view_h) s.offset = y + h - view_h;
    s.target = -1;
}

int tabs(Context& c, Id id, const Rect& r, const std::vector<std::string>& labels, int current,
         const std::vector<std::string>& badges) {
    auto& T = c.theme;
    float w = r.w / float(labels.size());
    int result = current;
    c.r.rect(r, T.bg);
    for (size_t i = 0; i < labels.size(); i++) {
        Rect tr{std::round(r.x + w * float(i)), r.y, std::round(w), r.h};
        bool sel = int(i) == current;
        bool hov = c.hovered(tr);
        if (c.clicked(id + i, tr)) result = int(i);
        if (hov) c.r.rect(tr, T.row_hover);
        std::string lbl = labels[i];
        float tw = c.text_w(FontId::Regular, 12, lbl);
        std::string badge_s = i < badges.size() ? badges[i] : "";
        // .counter: 9px semibold, padding 2px 5px, margin-left 4px, fully rounded
        float bw = badge_s.empty() ? 0 : c.text_w(FontId::Bold, 9, badge_s) + 10;
        float x = std::round(tr.x + (tr.w - tw - (bw ? bw + 4 : 0)) / 2);
        c.r.draw_text_in(FontId::Regular, 12, {x, tr.y, tw + 2, tr.h - 1}, lbl, T.text);
        if (bw) {
            float bx = x + tw + 4, by = std::round(tr.y + (tr.h - 13) / 2);
            c.r.rect({bx, by, bw, 13}, T.badge_bg.a > 0 ? (T.dark ? Color::hex(0x444d56) : T.badge_bg) : T.row_selected, 6.5f);
            c.r.draw_text_in(FontId::Bold, 9, {bx, by, bw, 13}, badge_s, T.text, Align::Center);
        }
        c.r.line_h(tr.x, tr.right(), tr.bottom() - 1, T.border);
        if (i + 1 < labels.size()) c.r.line_v(tr.right() - 1, tr.y, tr.bottom(), T.border);
        if (sel) c.r.rect({tr.x, tr.bottom() - 3, tr.w - (i + 1 < labels.size() ? 1 : 0), 3}, T.accent_emphasis);
        if (hov) c.set_cursor(plat::Cursor::Hand);
    }
    return result;
}

void segmented(Context& c, Id id, const Rect& r, const std::vector<std::string>& labels, int& current) {
    auto& T = c.theme;
    c.r.rect(r, T.bg_subtle, kRadius, 1, T.border);
    float w = r.w / float(labels.size());
    for (size_t i = 0; i < labels.size(); i++) {
        Rect tr{r.x + w * float(i), r.y, w, r.h};
        if (c.clicked(id + i, tr)) current = int(i);
        bool sel = int(i) == current;
        if (sel) c.r.rect(tr.inset(2), T.bg, kRadius - 1, 1, T.border);
        c.r.draw_text_in(sel ? FontId::Bold : FontId::Regular, 13, tr, labels[i], sel ? T.text : T.text_muted, Align::Center);
        if (c.hovered(tr)) c.set_cursor(plat::Cursor::Hand);
    }
}

// ================================================================ popups
static int g_popup_prev_layer = 0;

bool begin_popup(Context& c, Id id, const Rect& r) { return begin_popup(c, id, r, c.theme.bg, kRadius); }

bool begin_popup(Context& c, Id id, const Rect& r, Color bg, float radius) {
    if (!c.popup_open(id)) return false;
    g_popup_prev_layer = c.layer();
    c.set_layer(std::max(50 + c.popup_depth(id) * 2, c.layer() + 1));
    c.add_blocker(r, false);
    // close when clicking outside (but not on the frame it opened). Only the
    // topmost popup reacts; a click inside a child popup doesn't close parents.
    if (c.popup_is_top(id)) {
        if ((c.mouse_pressed[0] || c.mouse_pressed[1]) && !r.contains(c.mx, c.my) && !c.popup_just_opened()) {
            c.close_popup(id);
            c.set_layer(g_popup_prev_layer);
            return false;
        }
        if (c.key_pressed(Key::Escape)) {
            c.take_key(Key::Escape);
            c.close_popup(id);
            c.set_layer(g_popup_prev_layer);
            return false;
        }
    }
    // --base-box-shadow: 0 2px 7px var(--shadow-color)
    c.r.shadow({r.x, r.y + 2, r.w, r.h}, radius, 7, c.theme.shadow);
    c.r.rect(r, bg, radius, 1, c.theme.border);
    c.r.push_clip(r);
    return true;
}

void end_popup(Context& c) {
    c.r.pop_clip();
    c.set_layer(g_popup_prev_layer);
}

bool menu_item(Context& c, Id id, const Rect& r, const std::string& label, const std::string& shortcut, bool enabled,
               const char* icon, bool checked) {
    auto& T = c.theme;
    bool hov = enabled && c.hovered(r);
    bool click = enabled && c.clicked(id, r);
    if (hov) c.r.rect(r, T.row_selected_focus);
    Color fg = hov ? Color::hex(0xffffff) : enabled ? T.text : T.text_muted.with_alpha(0.6f);
    float x = r.x + 12;
    if (checked) c.r.icon("check", x, r.y + (r.h - 16) / 2, 16, fg);
    if (icon) { c.r.icon(icon, x, r.y + (r.h - 16) / 2, 16, hov ? fg : enabled ? T.text_muted : fg); }
    if (icon || checked) x += 24;
    c.r.draw_text_in(FontId::Regular, 12, {x, r.y, r.w - (x - r.x) - (shortcut.empty() ? 12 : 100), r.h}, label, fg);
    if (!shortcut.empty()) c.r.draw_text_in(FontId::Regular, 12, {r.right() - 100, r.y, 88, r.h}, shortcut, hov ? fg : T.text_muted, Align::Right);
    if (hov) c.set_cursor(plat::Cursor::Hand);
    if (click) c.close_all_popups();
    return click;
}

void separator(Context& c, const Rect& r) { c.r.line_h(r.x, r.right(), r.y + r.h / 2, c.theme.border); }

// ================================================================ modals
static uint64_t g_modal_frame = 0;
static int g_modal_count = 0;

Rect begin_modal(Context& c, Id id, const std::string& title, float w, float h, bool* closed) {
    auto& T = c.theme;
    if (g_modal_frame != c.frame()) { g_modal_frame = c.frame(); g_modal_count = 0; }
    int layer = 60 + g_modal_count++ * 2;
    c.set_layer(layer);
    float W = c.r.width(), H = c.r.height();
    w = std::min(w, W - 40);
    h = std::min(h, H - 40);
    Rect full{0, 0, W, H};
    c.add_blocker(full, true);
    c.r.rect(full, T.overlay);
    Rect box{std::round((W - w) / 2), std::round(std::max(20.f, (H - h) / 2.5f)), w, h};
    // ui/_dialog.scss: radius 6, base border, 50px header with a 14px semibold title
    c.r.shadow({box.x, box.y + 2, box.w, box.h}, kRadius, 7, T.shadow);
    c.r.rect(box, T.bg, kRadius, 1, T.border);
    Rect head{box.x, box.y, box.w, 50};
    c.r.draw_text_in(FontId::Bold, 14, {head.x + 20, head.y, head.w - 70, head.h}, title, T.text);
    if (closed) *closed = false;
    if (closed && icon_button(c, id ^ 0xc105e, {head.right() - 36, head.y + 13, 24, 24}, "x", "Close")) *closed = true;
    if (closed && c.key_pressed(Key::Escape)) {
        c.take_key(Key::Escape);
        *closed = true;
    }
    c.r.line_h(box.x, box.right(), head.bottom(), T.border);
    Rect content{box.x, head.bottom(), box.w, box.h - head.h};
    c.r.push_clip(box);
    return content;
}

void end_modal(Context& c) {
    c.r.pop_clip();
    c.set_layer(0);
}

// ================================================================ misc
void spinner(Context& c, float cx, float cy, float rad, Color col) {
    int n = 10;
    for (int i = 0; i < n; i++) {
        float a = float(i) / float(n) * 6.2831853f + float(c.time) * 6.f;
        float x = cx + std::cos(a) * rad, y = cy + std::sin(a) * rad;
        float alpha = float(i + 1) / float(n);
        c.r.rect({x - 1.5f, y - 1.5f, 3, 3}, col.with_alpha(alpha * col.a), 1.5f);
    }
    c.animate();
}

void progress_bar(Context& c, const Rect& r, float t, bool indeterminate) {
    c.r.rect(r, c.theme.row_selected, r.h / 2);
    if (indeterminate) {
        float w = r.w * 0.3f;
        float x = r.x + std::fmod(float(c.time) * r.w * 0.8f, r.w + w) - w;
        c.r.push_clip(r);
        c.r.rect({x, r.y, w, r.h}, c.theme.accent_emphasis, r.h / 2);
        c.r.pop_clip();
        c.animate();
    } else {
        c.r.rect({r.x, r.y, r.w * std::clamp(t, 0.f, 1.f), r.h}, c.theme.accent_emphasis, r.h / 2);
    }
}

void label(Context& c, const Rect& r, const std::string& text, FontId f, float size, Color col, Align a) {
    c.r.draw_text_in(f, size, r, text, col.a > 0 ? col : c.theme.text, a);
}

float wrapped_text(Context& c, const Rect& r, const std::string& text, FontId f, float size, Color col, float spacing,
                   bool measure_only) {
    auto lines = wrap_lines(c, f, size, text, r.w);
    float lh = c.r.text().line_height(f, size) * spacing / 1.2f;
    if (!measure_only)
        for (size_t i = 0; i < lines.size(); i++)
            c.r.draw_text(f, size, r.x, r.y + float(i) * lh, std::string_view(text).substr(lines[i].start, lines[i].end - lines[i].start), col);
    return float(lines.size()) * lh;
}

float splitter(Context& c, Id id, const Rect& r, float value, float min_v, float max_v) {
    Rect hit{r.x - 3, r.y, r.w + 6, r.h};
    bool hov = c.hovered(hit);
    if (hov || c.is_active(id)) c.set_cursor(plat::Cursor::ResizeH);
    if (c.pressed(id, hit)) {}
    if (c.is_active(id)) {
        if (c.mouse_down[0]) {
            value = std::clamp(c.mx, min_v, max_v);
            c.request_redraw();
        } else c.clear_active();
    }
    c.r.rect(r, c.is_active(id) ? c.theme.accent_emphasis : c.theme.border);
    return value;
}

void avatar(Context& c, const Rect& r, const std::string& name) {
    uint32_t h = 2166136261u;
    for (char ch : name) h = (h ^ uint8_t(ch)) * 16777619u;
    static const uint32_t palette[] = {0x0969da, 0x8250df, 0xbf3989, 0x1a7f37, 0x9a6700, 0xbc4c00, 0x1b7c83, 0xcf222e};
    Color bg = Color::hex(palette[h % 8]);
    c.r.rect(r, bg, r.w / 2);
    std::string initials;
    bool next = true;
    for (size_t i = 0; i < name.size() && initials.size() < 2; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == ' ' || ch == '-' || ch == '_' || ch == '.') { next = true; continue; }
        if (next && std::isalnum(ch)) { initials += char(std::toupper(ch)); next = false; }
    }
    if (initials.empty()) initials = "?";
    c.r.draw_text_in(FontId::Bold, r.h * 0.42f, r, initials, Color::hex(0xffffff), Align::Center);
}

void badge(Context& c, float x, float y, const std::string& text, Color bg, Color fg, float* w_out) {
    float w = c.text_w(FontId::Bold, 11, text) + 14;
    c.r.rect({x, y, w, 20}, bg, 10, 1, fg.with_alpha(0.35f));
    c.r.draw_text_in(FontId::Bold, 11, {x, y, w, 20}, text, fg, Align::Center);
    if (w_out) *w_out = w;
}

}  // namespace ui
