// SPDX-License-Identifier: MIT
#include "terminal.hpp"

#include <algorithm>
#include <cmath>

namespace ui {

using plat::Key;

// ================================================================ character width
// East Asian wide/fullwidth and emoji ranges take two cells (a compact wcwidth).
static int cp_width(uint32_t c) {
    if (c == 0) return 0;
    if (c < 0x300) return 1;
    // combining marks
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
        (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200B || c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    static const uint32_t wide[][2] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
        {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
        {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA},
        {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
        {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
        {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
        {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB},
        {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};
    for (auto& w : wide) if (c >= w[0] && c <= w[1]) return 2;
    return 1;
}

static void append_utf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
}

// DEC special graphics (ESC ( 0): line drawing used by curses apps
static uint32_t dec_graphics(uint32_t c) {
    static const uint16_t map[] = {0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C,
                                   0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264,
                                   0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};
    if (c >= 0x60 && c <= 0x7E) return map[c - 0x60];
    return c;
}

// ================================================================ Terminal
Terminal::Terminal(int cols, int rows) : cols_(std::max(2, cols)), rows_(std::max(1, rows)) { reset(); }

TermCell Terminal::blank() const {
    TermCell b;
    b.bg = pen_.bg;  // erase uses the current background (BCE), like xterm
    return b;
}

void Terminal::reset() {
    main_.assign(size_t(rows_), TermLine(size_t(cols_)));
    alt_.assign(size_t(rows_), TermLine(size_t(cols_)));
    alt_active_ = false;
    cx_ = cy_ = 0;
    wrap_pending_ = false;
    pen_ = TermCell{};
    top_ = 0;
    bottom_ = rows_ - 1;
    origin_ = insert_ = app_cursor_ = bracketed_paste_ = mouse_sgr_ = newline_mode_ = false;
    autowrap_ = cursor_visible_ = true;
    mouse_mode_ = 0;
    g0_lines_ = g1_lines_ = shift_out_ = false;
    tabs_.assign(size_t(cols_), false);
    for (int i = 8; i < cols_; i += 8) tabs_[size_t(i)] = true;
    st_ = St::Ground;
    version_++;
}

std::string Terminal::take_reply() {
    std::string r;
    r.swap(reply_);
    return r;
}

void Terminal::resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;
    auto fit = [&](Grid& g, bool is_main) {
        // keep the cursor on screen: lines above it scroll into history
        int extra = (is_main == !alt_active_ ? cy_ : 0) - (rows - 1);
        for (int i = 0; i < extra && !g.empty(); i++) {
            if (is_main) { scrollback_.push_back(g.front()); if (scrollback_.size() > max_scrollback_) scrollback_.pop_front(); }
            g.erase(g.begin());
        }
        if (is_main == !alt_active_ && extra > 0) cy_ -= extra;
        // pull history back down when growing, like most terminals
        while (is_main && int(g.size()) < rows && !scrollback_.empty() && !alt_active_) {
            g.insert(g.begin(), scrollback_.back());
            scrollback_.pop_back();
            cy_++;
        }
        g.resize(size_t(rows), TermLine(size_t(cols)));
        for (auto& l : g) l.resize(size_t(cols));
    };
    fit(main_, true);
    fit(alt_, false);
    for (auto& l : scrollback_) l.resize(size_t(cols));
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bottom_ = rows_ - 1;
    tabs_.assign(size_t(cols_), false);
    for (int i = 8; i < cols_; i += 8) tabs_[size_t(i)] = true;
    clamp_cursor();
    wrap_pending_ = false;
    version_++;
}

void Terminal::clamp_cursor() {
    cx_ = std::clamp(cx_, 0, cols_ - 1);
    cy_ = std::clamp(cy_, 0, rows_ - 1);
}

void Terminal::scroll_up(int top, int bottom, int n, bool history) {
    auto& g = grid();
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; i++) {
        if (history && !alt_active_ && top == 0) {
            scrollback_.push_back(g[size_t(top)]);
            if (scrollback_.size() > max_scrollback_) scrollback_.pop_front();
        }
        g.erase(g.begin() + top);
        g.insert(g.begin() + bottom, TermLine(size_t(cols_), blank()));
    }
}

void Terminal::scroll_down(int top, int bottom, int n) {
    auto& g = grid();
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; i++) {
        g.erase(g.begin() + bottom);
        g.insert(g.begin() + top, TermLine(size_t(cols_), blank()));
    }
}

void Terminal::linefeed() {
    wrap_pending_ = false;
    if (cy_ == bottom_) scroll_up(top_, bottom_, 1);
    else if (cy_ < rows_ - 1) cy_++;
}

void Terminal::erase_cells(int y, int x0, int x1) {
    auto& l = grid()[size_t(y)];
    for (int x = std::max(0, x0); x < std::min(cols_, x1); x++) l[size_t(x)] = blank();
}

void Terminal::put(uint32_t cp) {
    if ((shift_out_ ? g1_lines_ : g0_lines_)) cp = dec_graphics(cp);
    int w = cp_width(cp);
    if (w == 0) {
        // combining mark: attach to the previous cell (approximation: drop)
        return;
    }
    if (wrap_pending_ && autowrap_) {
        cx_ = 0;
        linefeed();
    }
    wrap_pending_ = false;
    auto& g = grid();
    if (w == 2 && cx_ == cols_ - 1) {
        if (autowrap_) { g[size_t(cy_)][size_t(cx_)] = blank(); cx_ = 0; linefeed(); }
        else return;
    }
    auto& l = g[size_t(cy_)];
    if (insert_) {
        for (int i = 0; i < w; i++) { l.insert(l.begin() + cx_, blank()); l.pop_back(); }
    }
    // overwriting half of a wide char clears the other half
    if (l[size_t(cx_)].width == 0 && cx_ > 0) l[size_t(cx_ - 1)] = blank();
    if (l[size_t(cx_)].width == 2 && cx_ + 1 < cols_) l[size_t(cx_ + 1)] = blank();
    TermCell c = pen_;
    c.cp = cp;
    c.width = uint8_t(w);
    l[size_t(cx_)] = c;
    if (w == 2) {
        TermCell c2 = pen_;
        c2.cp = 0;
        c2.width = 0;
        l[size_t(cx_ + 1)] = c2;
    }
    last_cp_ = cp;
    cx_ += w;
    if (cx_ >= cols_) {
        cx_ = cols_ - 1;
        wrap_pending_ = true;
    }
}

void Terminal::feed(std::string_view bytes) {
    for (unsigned char b : bytes) {
        // ---- strings (OSC, DCS/APC/PM): collect until BEL or ST
        if (st_ == St::Osc || st_ == St::Str) {
            if (b == 0x07) { if (st_ == St::Osc) osc(); st_ = St::Ground; continue; }
            if (b == 0x1B) { st_ = st_ == St::Osc ? St::OscEsc : St::StrEsc; continue; }
            if (st_ == St::Osc && osc_.size() < 4096) osc_ += char(b);
            continue;
        }
        if (st_ == St::OscEsc || st_ == St::StrEsc) {
            if (b == '\\') { if (st_ == St::OscEsc) osc(); st_ = St::Ground; continue; }
            if (st_ == St::OscEsc) { osc_ += '\x1b'; osc_ += char(b); st_ = St::Osc; } else st_ = St::Str;
            continue;
        }
        // ---- C0 controls act in every other state
        if (b < 0x20 || b == 0x7F) {
            switch (b) {
                case 0x07: bell_ = true; break;
                case 0x08: if (cx_ > 0) cx_--; wrap_pending_ = false; break;
                case 0x09: {
                    int x = cx_ + 1;
                    while (x < cols_ - 1 && !tabs_[size_t(x)]) x++;
                    cx_ = std::min(x, cols_ - 1);
                    break;
                }
                case 0x0A: case 0x0B: case 0x0C: linefeed(); if (newline_mode_) cx_ = 0; break;
                case 0x0D: cx_ = 0; wrap_pending_ = false; break;
                case 0x0E: shift_out_ = true; break;
                case 0x0F: shift_out_ = false; break;
                case 0x18: case 0x1A: st_ = St::Ground; break;
                case 0x1B:
                    st_ = St::Esc;
                    esc_inter_ = 0;
                    break;
                default: break;
            }
            utf8_need_ = 0;
            continue;
        }
        switch (st_) {
            case St::Ground: {
                if (utf8_need_) {
                    if ((b & 0xC0) == 0x80) {
                        utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3F);
                        if (--utf8_need_ == 0) put(utf8_cp_);
                        continue;
                    }
                    utf8_need_ = 0;
                    put(0xFFFD);
                }
                if (b < 0x80) put(b);
                else if ((b & 0xE0) == 0xC0) { utf8_cp_ = b & 0x1F; utf8_need_ = 1; }
                else if ((b & 0xF0) == 0xE0) { utf8_cp_ = b & 0x0F; utf8_need_ = 2; }
                else if ((b & 0xF8) == 0xF0) { utf8_cp_ = b & 0x07; utf8_need_ = 3; }
                else put(0xFFFD);
                break;
            }
            case St::Esc:
                switch (b) {
                    case '[': st_ = St::Csi; params_.clear(); sub_.clear(); have_param_ = false; priv_ = 0; inter_ = 0; break;
                    case ']': st_ = St::Osc; osc_.clear(); break;
                    case 'P': case '_': case '^': case 'X': st_ = St::Str; break;
                    case '(': case ')': case '*': case '+': case '#': case ' ': case '%':
                        esc_inter_ = char(b);
                        st_ = St::EscInter;
                        break;
                    case '7': saved_ = {cx_, cy_, pen_, origin_, g0_lines_}; st_ = St::Ground; break;
                    case '8': cx_ = saved_.x; cy_ = saved_.y; pen_ = saved_.pen; origin_ = saved_.origin; g0_lines_ = saved_.g0_lines;
                              clamp_cursor(); wrap_pending_ = false; st_ = St::Ground; break;
                    case 'D': linefeed(); st_ = St::Ground; break;
                    case 'E': cx_ = 0; linefeed(); st_ = St::Ground; break;
                    case 'H': tabs_[size_t(cx_)] = true; st_ = St::Ground; break;
                    case 'M':
                        if (cy_ == top_) scroll_down(top_, bottom_, 1);
                        else if (cy_ > 0) cy_--;
                        wrap_pending_ = false;
                        st_ = St::Ground;
                        break;
                    case 'c': reset(); st_ = St::Ground; break;
                    case '=': case '>': st_ = St::Ground; break;  // keypad modes
                    default: st_ = St::Ground; break;
                }
                break;
            case St::EscInter:
                if (esc_inter_ == '(') g0_lines_ = b == '0';
                else if (esc_inter_ == ')') g1_lines_ = b == '0';
                else if (esc_inter_ == '#' && b == '8') {  // DECALN: fill with E
                    for (auto& l : grid()) for (auto& c : l) { c = TermCell{}; c.cp = 'E'; }
                }
                st_ = St::Ground;
                break;
            case St::Csi:
                if (b >= '0' && b <= '9') {
                    if (!have_param_) { params_.push_back(0); sub_.emplace_back(); have_param_ = true; }
                    if (!sub_.back().empty()) sub_.back().back() = std::min(sub_.back().back() * 10 + (b - '0'), 65535);
                    else params_.back() = std::min(params_.back() * 10 + (b - '0'), 65535);
                } else if (b == ';') {
                    if (!have_param_) { params_.push_back(0); sub_.emplace_back(); }
                    have_param_ = false;
                } else if (b == ':') {
                    if (!have_param_) { params_.push_back(0); sub_.emplace_back(); have_param_ = true; }
                    sub_.back().push_back(0);
                } else if (b == '?' || b == '>' || b == '<' || b == '=') {
                    priv_ = char(b);
                } else if (b >= 0x20 && b <= 0x2F) {
                    inter_ = char(b);
                } else if (b >= 0x40 && b <= 0x7E) {
                    csi(char(b));
                    st_ = St::Ground;
                } else {
                    st_ = St::Ground;
                }
                break;
            default: st_ = St::Ground; break;
        }
    }
    version_++;
}

void Terminal::set_mode(bool priv, int p, bool on) {
    if (!priv) {
        if (p == 4) insert_ = on;
        else if (p == 20) newline_mode_ = on;
        return;
    }
    switch (p) {
        case 1: app_cursor_ = on; break;
        case 6: origin_ = on; cx_ = 0; cy_ = on ? top_ : 0; break;
        case 7: autowrap_ = on; break;
        case 25: cursor_visible_ = on; break;
        case 47: case 1047: case 1049:
            if (on == alt_active_) break;
            if (on) {
                if (p == 1049) saved_alt_ = {cx_, cy_, pen_, origin_, g0_lines_};
                alt_active_ = true;
                alt_.assign(size_t(rows_), TermLine(size_t(cols_)));
            } else {
                alt_active_ = false;
                if (p == 1049) { cx_ = saved_alt_.x; cy_ = saved_alt_.y; pen_ = saved_alt_.pen; clamp_cursor(); }
            }
            wrap_pending_ = false;
            break;
        case 1000: case 1002: case 1003: mouse_mode_ = on ? p : 0; break;
        case 1006: mouse_sgr_ = on; break;
        case 2004: bracketed_paste_ = on; break;
        default: break;
    }
}

void Terminal::csi(char f) {
    auto& g = grid();
    if (priv_ == '?' && (f == 'h' || f == 'l')) {
        for (size_t i = 0; i < std::max<size_t>(1, params_.size()); i++) set_mode(true, i < params_.size() ? params_[i] : 0, f == 'h');
        return;
    }
    if (priv_ == '>' || priv_ == '=' || priv_ == '<') {
        if (f == 'c' && priv_ == '>') reply_ += "\x1b[>0;10;1c";
        return;  // xterm modifyOtherKeys etc.
    }
    if (inter_ == ' ' && f == 'q') return;  // cursor style
    if (inter_ == '!' && f == 'p') { reset(); return; }  // DECSTR (soft reset)
    int n = param(0, 1);
    switch (f) {
        case 'A': cy_ = std::max(cy_ - n, cy_ >= top_ ? top_ : 0); wrap_pending_ = false; break;
        case 'B': case 'e': cy_ = std::min(cy_ + n, cy_ <= bottom_ ? bottom_ : rows_ - 1); wrap_pending_ = false; break;
        case 'C': case 'a': cx_ = std::min(cx_ + n, cols_ - 1); wrap_pending_ = false; break;
        case 'D': cx_ = std::max(cx_ - n, 0); wrap_pending_ = false; break;
        case 'E': cx_ = 0; cy_ = std::min(cy_ + n, bottom_); wrap_pending_ = false; break;
        case 'F': cx_ = 0; cy_ = std::max(cy_ - n, top_); wrap_pending_ = false; break;
        case 'G': case '`': cx_ = std::clamp(n - 1, 0, cols_ - 1); wrap_pending_ = false; break;
        case 'd': cy_ = std::clamp((origin_ ? top_ : 0) + n - 1, 0, rows_ - 1); wrap_pending_ = false; break;
        case 'H': case 'f': {
            int row = param(0, 1), col = param(1, 1);
            cy_ = std::clamp((origin_ ? top_ : 0) + row - 1, 0, origin_ ? bottom_ : rows_ - 1);
            cx_ = std::clamp(col - 1, 0, cols_ - 1);
            wrap_pending_ = false;
            break;
        }
        case 'J': {
            int m = params_.empty() ? 0 : params_[0];
            if (m == 0) { erase_cells(cy_, cx_, cols_); for (int y = cy_ + 1; y < rows_; y++) erase_cells(y, 0, cols_); }
            else if (m == 1) { erase_cells(cy_, 0, cx_ + 1); for (int y = 0; y < cy_; y++) erase_cells(y, 0, cols_); }
            else if (m == 2) { for (int y = 0; y < rows_; y++) erase_cells(y, 0, cols_); }
            else if (m == 3) { scrollback_.clear(); }
            break;
        }
        case 'K': {
            int m = params_.empty() ? 0 : params_[0];
            if (m == 0) erase_cells(cy_, cx_, cols_);
            else if (m == 1) erase_cells(cy_, 0, cx_ + 1);
            else erase_cells(cy_, 0, cols_);
            break;
        }
        case 'L': if (cy_ >= top_ && cy_ <= bottom_) scroll_down(cy_, bottom_, n); cx_ = 0; break;
        case 'M': if (cy_ >= top_ && cy_ <= bottom_) scroll_up(cy_, bottom_, n, false); cx_ = 0; break;
        case 'P': {
            auto& l = g[size_t(cy_)];
            n = std::min(n, cols_ - cx_);
            l.erase(l.begin() + cx_, l.begin() + cx_ + n);
            l.insert(l.end(), size_t(n), blank());
            break;
        }
        case '@': {
            auto& l = g[size_t(cy_)];
            n = std::min(n, cols_ - cx_);
            l.insert(l.begin() + cx_, size_t(n), blank());
            l.resize(size_t(cols_));
            break;
        }
        case 'X': erase_cells(cy_, cx_, cx_ + n); break;
        case 'S': scroll_up(top_, bottom_, n, false); break;
        case 'T': scroll_down(top_, bottom_, n); break;
        case 'b': for (int i = 0; i < n && i < 1000; i++) put(last_cp_); break;
        case 'm': sgr(); break;
        case 'r': {
            int t = param(0, 1) - 1, bt = param(1, rows_) - 1;
            if (t < bt && bt < rows_) { top_ = t; bottom_ = bt; }
            else { top_ = 0; bottom_ = rows_ - 1; }
            cx_ = 0;
            cy_ = origin_ ? top_ : 0;
            wrap_pending_ = false;
            break;
        }
        case 's': saved_ = {cx_, cy_, pen_, origin_, g0_lines_}; break;
        case 'u': cx_ = saved_.x; cy_ = saved_.y; pen_ = saved_.pen; clamp_cursor(); wrap_pending_ = false; break;
        case 'h': case 'l': for (auto p : params_) set_mode(false, p, f == 'h'); break;
        case 'g': {
            int m = params_.empty() ? 0 : params_[0];
            if (m == 0) tabs_[size_t(cx_)] = false;
            else if (m == 3) std::fill(tabs_.begin(), tabs_.end(), false);
            break;
        }
        case 'n':
            if (params_.size() && params_[0] == 5) reply_ += "\x1b[0n";
            else if (params_.size() && params_[0] == 6)
                reply_ += "\x1b[" + std::to_string(cy_ - (origin_ ? top_ : 0) + 1) + ";" + std::to_string(cx_ + 1) + "R";
            break;
        case 'c': if (param(0, 0) == 0) reply_ += "\x1b[?62;22c"; break;
        case 't':
            // window size reports, some apps ask: 18 → text area in characters
            if (params_.size() && params_[0] == 18) reply_ += "\x1b[8;" + std::to_string(rows_) + ";" + std::to_string(cols_) + "t";
            break;
        default: break;
    }
}

void Terminal::sgr() {
    if (params_.empty()) { pen_.fg = pen_.bg = kTermDefault; pen_.attr = 0; return; }
    for (size_t i = 0; i < params_.size(); i++) {
        int p = params_[i];
        auto color = [&](uint32_t& dst) {
            // 38;5;n  38;2;r;g;b  and the colon forms 38:5:n  38:2::r:g:b
            auto& sp = sub_[i];
            if (!sp.empty()) {
                if (sp[0] == 5 && sp.size() >= 2) dst = uint32_t(std::clamp(sp[1], 0, 255));
                else if (sp[0] == 2 && sp.size() >= 4) {
                    size_t o = sp.size() >= 5 ? 2 : 1;
                    dst = kTermRgb | uint32_t(std::clamp(sp[o], 0, 255)) << 16 | uint32_t(std::clamp(sp[o + 1], 0, 255)) << 8 | uint32_t(std::clamp(sp[o + 2], 0, 255));
                }
                return;
            }
            if (i + 1 >= params_.size()) return;
            if (params_[i + 1] == 5 && i + 2 < params_.size()) { dst = uint32_t(std::clamp(params_[i + 2], 0, 255)); i += 2; }
            else if (params_[i + 1] == 2 && i + 4 < params_.size()) {
                dst = kTermRgb | uint32_t(std::clamp(params_[i + 2], 0, 255)) << 16 | uint32_t(std::clamp(params_[i + 3], 0, 255)) << 8 |
                      uint32_t(std::clamp(params_[i + 4], 0, 255));
                i += 4;
            }
        };
        switch (p) {
            case 0: pen_.fg = pen_.bg = kTermDefault; pen_.attr = 0; break;
            case 1: pen_.attr |= 1; break;
            case 2: pen_.attr |= 2; break;
            case 3: pen_.attr |= 4; break;
            case 4: pen_.attr = uint8_t(sub_[i].empty() || sub_[i][0] != 0 ? pen_.attr | 8 : pen_.attr & ~8); break;
            case 7: pen_.attr |= 16; break;
            case 8: pen_.attr |= 64; break;
            case 9: pen_.attr |= 32; break;
            case 21: pen_.attr |= 8; break;
            case 22: pen_.attr &= uint8_t(~3); break;
            case 23: pen_.attr &= uint8_t(~4); break;
            case 24: pen_.attr &= uint8_t(~8); break;
            case 27: pen_.attr &= uint8_t(~16); break;
            case 28: pen_.attr &= uint8_t(~64); break;
            case 29: pen_.attr &= uint8_t(~32); break;
            case 38: color(pen_.fg); break;
            case 39: pen_.fg = kTermDefault; break;
            case 48: color(pen_.bg); break;
            case 49: pen_.bg = kTermDefault; break;
            default:
                if (p >= 30 && p <= 37) pen_.fg = uint32_t(p - 30);
                else if (p >= 40 && p <= 47) pen_.bg = uint32_t(p - 40);
                else if (p >= 90 && p <= 97) pen_.fg = uint32_t(p - 90 + 8);
                else if (p >= 100 && p <= 107) pen_.bg = uint32_t(p - 100 + 8);
        }
    }
}

void Terminal::osc() {
    auto semi = osc_.find(';');
    std::string code = osc_.substr(0, semi), arg = semi == std::string::npos ? "" : osc_.substr(semi + 1);
    if (code == "0" || code == "2") title_ = arg;
    else if (code == "7") {
        // file://host/path
        auto p = arg.find("://");
        if (p != std::string::npos) {
            auto slash = arg.find('/', p + 3);
            if (slash != std::string::npos) cwd_ = arg.substr(slash);
        }
    } else if (code == "10" && arg == "?") reply_ += "\x1b]10;rgb:cccc/cccc/cccc\x1b\\";
    else if (code == "11" && arg == "?") reply_ += "\x1b]11;rgb:2424/2929/2e2e\x1b\\";
}

}  // namespace ui

// ================================================================ widget
namespace ui {

static Color term_color(const Theme& T, uint32_t c, bool fg) {
    if (c == kTermDefault) return fg ? T.text : T.bg;
    if (c & kTermRgb) return Color::hex(c & 0xFFFFFF);
    // Primer's terminal palette (github.com Codespaces), light and dark
    static const uint32_t dark[16] = {0x484f58, 0xff7b72, 0x3fb950, 0xd29922, 0x58a6ff, 0xbc8cff, 0x39c5cf, 0xb1bac4,
                                      0x6e7681, 0xffa198, 0x56d364, 0xe3b341, 0x79c0ff, 0xd2a8ff, 0x56d4dd, 0xffffff};
    static const uint32_t light[16] = {0x24292f, 0xcf222e, 0x116329, 0x4d2d00, 0x0969da, 0x8250df, 0x1b7c83, 0x6e7781,
                                       0x57606a, 0xa40e26, 0x1a7f37, 0x633c01, 0x218bff, 0xa475f9, 0x3192aa, 0x8c959f};
    if (c < 16) return Color::hex(T.dark ? dark[c] : light[c]);
    if (c < 232) {
        int i = int(c) - 16;
        auto lv = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return Color{float(lv(i / 36)) / 255.f, float(lv((i / 6) % 6)) / 255.f, float(lv(i % 6)) / 255.f, 1.f};
    }
    float g = float(8 + (int(c) - 232) * 10) / 255.f;
    return Color{g, g, g, 1.f};
}

static std::string key_sequence(const Terminal& t, Key k, uint8_t mods) {
    bool shift = mods & plat::ModShift, alt = mods & plat::ModAlt, ctrl = mods & plat::ModCtrl;
    int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    auto csi_mod = [&](const char* plain_app, const char* plain, char final) -> std::string {
        if (m > 1) return std::string("\x1b[1;") + std::to_string(m) + final;
        return t.app_cursor() ? plain_app : plain;
    };
    auto tilde = [&](int code) -> std::string {
        return m > 1 ? "\x1b[" + std::to_string(code) + ";" + std::to_string(m) + "~" : "\x1b[" + std::to_string(code) + "~";
    };
    switch (k) {
        case Key::Enter: return alt ? "\x1b\r" : "\r";
        case Key::Backspace: return ctrl ? "\x08" : alt ? "\x1b\x7f" : "\x7f";
        case Key::Tab: return shift ? "\x1b[Z" : "\t";
        case Key::Escape: return "\x1b";
        case Key::Up: return csi_mod("\x1bOA", "\x1b[A", 'A');
        case Key::Down: return csi_mod("\x1bOB", "\x1b[B", 'B');
        case Key::Right: return csi_mod("\x1bOC", "\x1b[C", 'C');
        case Key::Left: return csi_mod("\x1bOD", "\x1b[D", 'D');
        case Key::Home: return csi_mod("\x1bOH", "\x1b[H", 'H');
        case Key::End: return csi_mod("\x1bOF", "\x1b[F", 'F');
        case Key::Insert: return tilde(2);
        case Key::Delete: return tilde(3);
        case Key::PageUp: return tilde(5);
        case Key::PageDown: return tilde(6);
        case Key::F1: return m > 1 ? "\x1b[1;" + std::to_string(m) + "P" : "\x1bOP";
        case Key::F2: return m > 1 ? "\x1b[1;" + std::to_string(m) + "Q" : "\x1bOQ";
        case Key::F3: return m > 1 ? "\x1b[1;" + std::to_string(m) + "R" : "\x1bOR";
        case Key::F4: return m > 1 ? "\x1b[1;" + std::to_string(m) + "S" : "\x1bOS";
        case Key::F5: return tilde(15);
        case Key::F6: return tilde(17);
        case Key::F7: return tilde(18);
        case Key::F8: return tilde(19);
        case Key::F9: return tilde(20);
        case Key::F10: return tilde(21);
        case Key::F11: return tilde(23);
        case Key::F12: return tilde(24);
        default: break;
    }
    if (ctrl && !alt) {
        if (k >= Key::A && k <= Key::Z) return std::string(1, char(1 + int(k) - int(Key::A)));
        switch (k) {
            case Key::Space: return std::string(1, '\0');
            case Key::LeftBracket: return "\x1b";
            case Key::Backslash: return "\x1c";
            case Key::RightBracket: return "\x1d";
            case Key::Slash: return "\x1f";
            case Key::Num2: return std::string(1, '\0');
            case Key::Num6: return "\x1e";
            case Key::Minus: return "\x1f";
            default: break;
        }
    }
#ifndef __APPLE__
    // Alt as Meta (macOS keeps Option for typing accented characters)
    if (alt && !ctrl) {
        if (k >= Key::A && k <= Key::Z) return std::string("\x1b") + char((shift ? 'A' : 'a') + int(k) - int(Key::A));
        if (k >= Key::Num0 && k <= Key::Num9) return std::string("\x1b") + char('0' + int(k) - int(Key::Num0));
        if (k == Key::Period) return "\x1b.";
    }
#endif
    return "";
}

static std::string paste_text(const Terminal& t, std::string s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out += s[i] == '\n' ? '\r' : s[i];
    }
    if (t.bracketed_paste()) {
        // never let pasted text end bracketed paste early
        size_t p;
        while ((p = out.find("\x1b[201~")) != std::string::npos) out.erase(p, 6);
        return "\x1b[200~" + out + "\x1b[201~";
    }
    return out;
}

bool terminal_view(Context& c, Id id, const Rect& r, TerminalSession& s, const std::function<void()>& wake) {
    auto& T = c.theme;
    const float fs = 12.5f;
    const float cw = c.text_w(FontId::Mono, fs, "M");
    const float lh = std::round(fs * 1.35f);
    const float pad = 8;
    int cols = std::max(10, int((r.w - 2 * pad - 10) / cw));
    int rows = std::max(2, int((r.h - 2 * pad) / lh));
    auto& t = s.term;
    bool changed = false;

    if (!s.started) {
        s.started = true;
        t.resize(cols, rows);
        auto argv = s.argv.empty() ? plat::default_shell() : s.argv;
        s.pty = plat::Pty::spawn(argv, s.cwd, cols, rows, &s.error);
        if (s.pty) s.pty->on_data = wake;
        else { s.exited = true; t.feed("\x1b[31m" + s.error + "\x1b[0m\r\n"); }
    }
    if (t.cols() != cols || t.rows() != rows) {
        t.resize(cols, rows);
        if (s.pty) s.pty->resize(cols, rows);
    }
    if (s.pty) {
        std::string out = s.pty->take_output();
        if (!out.empty()) {
            t.feed(out);
            changed = true;
            s.last_activity = c.time;
            if (s.scroll > 0) s.scroll = std::min<int>(s.scroll + 0, int(t.scrollback().size()));
        }
        if (auto rep = t.take_reply(); !rep.empty()) s.pty->write(rep);
        if (!s.pty->alive() && !s.exited) {
            s.exited = true;
            s.exit_code = s.pty->exit_code();
            t.feed("\r\n\x1b[2m[process exited with code " + std::to_string(s.exit_code) + "]\x1b[0m\r\n");
            changed = true;
        }
    }
    t.bell();

    c.register_focusable(id);
    c.capture_tab(id);
    bool focused = c.has_focus(id);
    bool hov = c.hovered(r);
    if (hov) c.set_cursor(plat::Cursor::IBeam);
    long sb = long(t.scrollback().size());
    auto cell_at = [&](float mx, float my) {
        int col = std::clamp(int((mx - r.x - pad) / cw), 0, cols - 1);
        int row = std::clamp(int((my - r.y - pad) / lh), 0, rows - 1);
        return TerminalSession::Pos{sb - s.scroll + row, col};
    };
    auto line_at = [&](long abs) -> const TermLine* {
        if (abs < 0) return nullptr;
        if (abs < sb) return &t.scrollback()[size_t(abs)];
        long y = abs - sb;
        if (y < t.rows()) return &t.line(int(y));
        return nullptr;
    };
    auto mouse_report = [&](int button, bool press, float mx, float my) {
        if (!s.pty || !t.mouse_mode()) return false;
        int col = std::clamp(int((mx - r.x - pad) / cw), 0, cols - 1) + 1;
        int row = std::clamp(int((my - r.y - pad) / lh), 0, rows - 1) + 1;
        if (t.mouse_sgr()) s.pty->write("\x1b[<" + std::to_string(button) + ";" + std::to_string(col) + ";" + std::to_string(row) + (press ? "M" : "m"));
        else if (press || true) {
            std::string seq = "\x1b[M";
            seq += char(32 + (press ? button : 3));
            seq += char(32 + std::min(col, 223));
            seq += char(32 + std::min(row, 223));
            s.pty->write(seq);
        }
        return true;
    };

    // ---------------- mouse
    bool app_mouse = t.mouse_mode() && !(c.mods & plat::ModShift);  // Shift bypasses app mouse, like xterm
    if (hov && c.mouse_pressed[0]) {
        c.set_focus(id);
        focused = true;
        if (app_mouse) mouse_report(0, true, c.mx, c.my);
        else {
            c.set_active(id);
            auto p = cell_at(c.mx, c.my);
            if (c.clicks >= 3) { s.sel_a = {p.line, 0}; s.sel_b = {p.line, cols}; s.has_sel = true; s.selecting = false; }
            else if (c.clicks == 2) {
                const TermLine* l = line_at(p.line);
                int a = p.col, b = p.col;
                auto word = [&](int x) { if (!l || x < 0 || x >= int(l->size())) return false; uint32_t ch = (*l)[size_t(x)].cp;
                                         return ch > ' ' && std::string("\"'`()[]{}<>|,;").find(char(ch < 128 ? ch : 'a')) == std::string::npos; };
                while (word(a - 1)) a--;
                while (word(b)) b++;
                s.sel_a = {p.line, a};
                s.sel_b = {p.line, b};
                s.has_sel = b > a;
                s.selecting = false;
            } else {
                s.sel_a = s.sel_b = p;
                s.has_sel = false;
                s.selecting = true;
            }
        }
    }
    if (app_mouse && hov && c.mouse_released[0]) mouse_report(0, false, c.mx, c.my);
    if (c.is_active(id) && s.selecting && c.mouse_down[0]) {
        s.sel_b = cell_at(c.mx, c.my);
        if (c.my > r.bottom() - 4 && s.scroll > 0) s.scroll--;
        if (c.my < r.y + 4 && s.scroll < sb) s.scroll++;
        s.has_sel = s.sel_a.line != s.sel_b.line || s.sel_a.col != s.sel_b.col;
        c.request_redraw();
    }
    if (c.is_active(id) && c.mouse_released[0]) { c.clear_active(); s.selecting = false; }
    if (hov && c.scroll_y != 0) {
        int lines = int(std::round(c.scroll_y / lh * 1.5f));
        if (lines == 0) lines = c.scroll_y > 0 ? 1 : -1;
        if (app_mouse) {
            for (int i = 0; i < std::abs(lines); i++) mouse_report(lines > 0 ? 64 : 65, true, c.mx, c.my);
        } else if (t.alt_screen()) {
            // alternate scroll: wheel becomes arrow keys in full-screen apps (less, vim)
            if (s.pty) for (int i = 0; i < std::abs(lines); i++) s.pty->write(lines > 0 ? (t.app_cursor() ? "\x1bOA" : "\x1b[A") : (t.app_cursor() ? "\x1bOB" : "\x1b[B"));
        } else {
            s.scroll = std::clamp<int>(s.scroll + lines, 0, int(sb));
        }
        c.scroll_y = 0;
    }

    // ---------------- selection text
    auto selected_text = [&]() {
        auto a = s.sel_a, b = s.sel_b;
        if (b.line < a.line || (b.line == a.line && b.col < a.col)) std::swap(a, b);
        std::string out;
        for (long ln = a.line; ln <= b.line; ln++) {
            const TermLine* l = line_at(ln);
            if (!l) continue;
            int x0 = ln == a.line ? a.col : 0, x1 = ln == b.line ? b.col : int(l->size());
            std::string row;
            for (int x = x0; x < std::min(x1, int(l->size())); x++) if ((*l)[size_t(x)].width) append_utf8(row, (*l)[size_t(x)].cp);
            while (!row.empty() && row.back() == ' ') row.pop_back();
            out += row;
            if (ln != b.line) out += '\n';
        }
        return out;
    };

    // ---------------- keyboard
    if (focused && s.pty && !s.exited) {
#ifdef __APPLE__
        const uint8_t COPY = plat::ModSuper, PASTE = plat::ModSuper;
#else
        const uint8_t COPY = uint8_t(plat::ModCtrl | plat::ModShift), PASTE = uint8_t(plat::ModCtrl | plat::ModShift);
#endif
        auto keys = c.keys;
        bool sent_text_key = false;
        for (auto& k : keys) {
            bool handled = true;
            if (k.key == Key::C && (k.mods == COPY || (k.mods == plat::ModCtrl && s.has_sel))) {
                if (s.has_sel) { c.set_clipboard(selected_text()); s.has_sel = false; }
                else if (k.mods == plat::ModCtrl) s.pty->write("\x03");
            } else if ((k.key == Key::V && k.mods == PASTE) || (k.key == Key::Insert && k.mods == plat::ModShift)) {
                std::string clip = c.clipboard();
                if (!clip.empty()) s.pty->write(paste_text(t, clip));
                s.scroll = 0;
#ifdef __APPLE__
            } else if (k.key == Key::K && k.mods == plat::ModSuper) {
                s.pty->write("\x0c");
#endif
            } else if ((k.key == Key::PageUp || k.key == Key::PageDown) && k.mods == plat::ModShift && !t.alt_screen()) {
                s.scroll = std::clamp<int>(s.scroll + (k.key == Key::PageUp ? rows - 1 : -(rows - 1)), 0, int(sb));
            } else if (k.mods & plat::ModSuper) {
                handled = false;  // leave app shortcuts alone
            } else {
                std::string seq = key_sequence(t, k.key, k.mods);
                if (seq.empty()) handled = false;
                else {
                    s.pty->write(seq);
                    s.scroll = 0;
                    s.has_sel = false;
                    if (k.mods & (plat::ModAlt | plat::ModCtrl)) sent_text_key = true;
                }
            }
            if (handled) c.take_key(k.key, k.mods);
        }
        if (!c.text_input.empty()) {
            // control/meta combos above already sent their bytes; drop the text they produce
            if (!sent_text_key) {
                s.pty->write(c.text_input);
                s.scroll = 0;
                s.has_sel = false;
            }
            c.text_input.clear();
        }
    }

    // ---------------- draw
    c.r.rect(r, T.bg);
    c.r.push_clip(r);
    auto norm = [&](TerminalSession::Pos a, TerminalSession::Pos b) {
        if (b.line < a.line || (b.line == a.line && b.col < a.col)) std::swap(a, b);
        return std::make_pair(a, b);
    };
    auto [sa, sbp] = norm(s.sel_a, s.sel_b);
    for (int vy = 0; vy < rows; vy++) {
        long abs = sb - s.scroll + vy;
        const TermLine* l = line_at(abs);
        if (!l) continue;
        float y = r.y + pad + float(vy) * lh;
        bool cursor_row = s.scroll == 0 && vy == t.cursor_y();
        std::string run;
        float run_x = 0;
        Color run_fg{};
        uint8_t run_attr = 0;
        auto flush = [&]() {
            if (run.empty()) return;
            c.r.draw_text(FontId::Mono, fs, run_x, y + (lh - fs) / 2 - 1, run, run_fg);
            run.clear();
        };
        for (int x = 0; x < int(l->size()) && x < cols; x++) {
            const TermCell& cell = (*l)[size_t(x)];
            if (cell.width == 0) continue;
            bool inv = cell.attr & 16;
            Color fg = term_color(T, inv ? cell.bg : cell.fg, !inv), bg = term_color(T, inv ? cell.fg : cell.bg, inv);
            if (inv && cell.bg == kTermDefault) fg = T.bg;
            if (inv && cell.fg == kTermDefault) bg = T.text;
            if ((cell.attr & 1) && cell.fg < 8 && !inv) fg = term_color(T, cell.fg + 8, true);  // bold brightens
            if (cell.attr & 2) fg = fg.with_alpha(0.6f);
            bool selected = s.has_sel && (abs > sa.line || (abs == sa.line && x >= sa.col)) && (abs < sbp.line || (abs == sbp.line && x < sbp.col));
            float cx = r.x + pad + float(x) * cw, wcells = float(cell.width);
            if (selected) c.r.rect({cx, y, cw * wcells, lh}, T.accent_emphasis.with_alpha(focused ? 0.35f : 0.2f));
            else if (inv || cell.bg != kTermDefault) c.r.rect({cx, y, cw * wcells, lh}, bg);
            bool is_cursor = cursor_row && x == t.cursor_x() && t.cursor_visible() && focused && !s.exited;
            if (is_cursor) { c.r.rect({cx, y, cw * wcells, lh}, T.text); fg = T.bg; }
            if (cell.attr & 64) { flush(); continue; }
            uint32_t cp = cell.cp;
            bool simple = cp < 0x80 && cell.width == 1;
            if (!simple || is_cursor || fg.r != run_fg.r || fg.g != run_fg.g || fg.b != run_fg.b || fg.a != run_fg.a || run_attr != cell.attr) flush();
            if (cp != ' ' || (cell.attr & 8)) {
                if (run.empty()) { run_x = cx; run_fg = fg; run_attr = cell.attr; }
                append_utf8(run, cp == 0 ? ' ' : cp);
                if (!simple || is_cursor) flush();
            } else {
                flush();
            }
            if (cell.attr & 8) c.r.line_h(cx, cx + cw * wcells, y + lh - 2, fg);
            if (cell.attr & 32) c.r.line_h(cx, cx + cw * wcells, y + lh / 2, fg);
        }
        flush();
        // hollow cursor when unfocused (or past the end of a line)
        if (cursor_row && t.cursor_visible() && !s.exited) {
            float cx = r.x + pad + float(t.cursor_x()) * cw;
            if (!focused) c.r.rect({cx, y, cw, lh}, Color{0, 0, 0, 0}, 0, 1, T.text_muted);
        }
    }
    // scrollback indicator
    if (sb > 0 && (s.scroll > 0 || hov)) {
        float total = float(sb + rows);
        float th = std::max(20.f, r.h * float(rows) / total);
        float ty = r.y + (r.h - th) * float(sb - s.scroll) / float(sb);
        c.r.rect({r.right() - 8, ty, 5, th}, T.scrollbar, 2.5f);
    }
    c.r.pop_clip();
    if (focused) c.win.set_text_input_rect(r.x + pad + float(t.cursor_x()) * cw, r.y + pad + float(t.cursor_y()) * lh, cw, lh);
    return changed;
}

}  // namespace ui
