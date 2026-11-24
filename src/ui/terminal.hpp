// SPDX-License-Identifier: MIT
// An xterm-compatible terminal emulator (VT100/VT220 + common xterm extensions:
// 256 colors and truecolor, alternate screen, scroll regions, bracketed paste,
// SGR mouse reporting, OSC titles) and the widget that hosts a shell in it.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ui.hpp"

namespace ui {

constexpr uint32_t kTermDefault = 0xFFFFFFFFu;  // default fg/bg
constexpr uint32_t kTermRgb = 0x01000000u;      // flag: value is 0xRRGGBB truecolor

struct TermCell {
    uint32_t cp = ' ';
    uint32_t fg = kTermDefault, bg = kTermDefault;
    uint8_t attr = 0;   // bold 1, dim 2, italic 4, underline 8, inverse 16, strike 32, hidden 64
    uint8_t width = 1;  // 2 for the first half of a wide character, 0 for its second half
};
using TermLine = std::vector<TermCell>;

class Terminal {
public:
    Terminal(int cols = 80, int rows = 24);
    void feed(std::string_view bytes);
    void resize(int cols, int rows);
    std::string take_reply();  // answers to queries (cursor position, device attributes)

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const TermLine& line(int y) const { return (alt_active_ ? alt_ : main_)[size_t(y)]; }
    const std::deque<TermLine>& scrollback() const { return scrollback_; }
    int cursor_x() const { return cx_; }
    int cursor_y() const { return cy_; }
    bool cursor_visible() const { return cursor_visible_; }
    bool alt_screen() const { return alt_active_; }
    bool app_cursor() const { return app_cursor_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    int mouse_mode() const { return mouse_mode_; }  // 0 off, 1000, 1002, 1003
    bool mouse_sgr() const { return mouse_sgr_; }
    const std::string& title() const { return title_; }
    const std::string& cwd() const { return cwd_; }
    uint64_t version() const { return version_; }
    bool bell() { bool b = bell_; bell_ = false; return b; }
    void reset();

private:
    using Grid = std::vector<TermLine>;
    Grid& grid() { return alt_active_ ? alt_ : main_; }
    TermCell blank() const;
    void put(uint32_t cp);
    void linefeed();
    void scroll_up(int top, int bottom, int n, bool history = true);
    void scroll_down(int top, int bottom, int n);
    void clamp_cursor();
    void csi(char final);
    void sgr();
    void set_mode(bool priv, int p, bool on);
    void osc();
    void erase_cells(int y, int x0, int x1);
    int param(size_t i, int def) const { return i < params_.size() && params_[i] > 0 ? params_[i] : def; }

    int cols_, rows_;
    Grid main_, alt_;
    bool alt_active_ = false;
    std::deque<TermLine> scrollback_;
    size_t max_scrollback_ = 5000;
    int cx_ = 0, cy_ = 0;
    bool wrap_pending_ = false;
    struct Saved { int x = 0, y = 0; TermCell pen; bool origin = false; bool g0_lines = false; } saved_, saved_alt_;
    TermCell pen_;
    int top_ = 0, bottom_ = 0;
    bool origin_ = false, autowrap_ = true, insert_ = false, app_cursor_ = false, cursor_visible_ = true;
    bool bracketed_paste_ = false, mouse_sgr_ = false, newline_mode_ = false;
    int mouse_mode_ = 0;
    bool g0_lines_ = false, g1_lines_ = false, shift_out_ = false;
    std::vector<bool> tabs_;
    std::string title_, cwd_, reply_;
    bool bell_ = false;
    uint64_t version_ = 0;
    uint32_t last_cp_ = ' ';

    // parser
    enum class St { Ground, Esc, EscInter, Csi, Osc, OscEsc, Str, StrEsc } st_ = St::Ground;
    std::vector<int> params_;
    std::vector<std::vector<int>> sub_;  // ':'-separated sub-parameters per param (SGR 38:2::r:g:b)
    bool have_param_ = false;
    char priv_ = 0, inter_ = 0, esc_inter_ = 0;
    std::string osc_;
    uint32_t utf8_cp_ = 0;
    int utf8_need_ = 0;
};

// A shell (or any program) running in a pseudo-terminal, shown in a Terminal.
struct TerminalSession {
    std::unique_ptr<plat::Pty> pty;
    Terminal term;
    std::string name = "Terminal", error;
    std::vector<std::string> argv;
    std::string cwd;
    int scroll = 0;  // lines scrolled back into history
    struct Pos { long line = 0; int col = 0; };  // absolute line index (scrollback + screen)
    Pos sel_a, sel_b;
    bool has_sel = false, selecting = false;
    bool started = false;
    int exit_code = 0;
    bool exited = false;
    double last_activity = 0;
};

// Starts the shell on first draw. `wake` is called from the pty thread when output arrives.
bool terminal_view(Context& c, Id id, const Rect& r, TerminalSession& s, const std::function<void()>& wake);

}  // namespace ui
