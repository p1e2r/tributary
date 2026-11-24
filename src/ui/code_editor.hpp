// SPDX-License-Identifier: MIT
// A code editor widget: line-based document, selection, undo/redo, auto-indent,
// line comments, and lightweight syntax highlighting.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui.hpp"

namespace ui {

struct CodePos {
    size_t line = 0, col = 0;  // col is a byte offset into the line
    bool operator==(const CodePos& o) const { return line == o.line && col == o.col; }
    bool operator!=(const CodePos& o) const { return !(*this == o); }
    bool operator<(const CodePos& o) const { return line < o.line || (line == o.line && col < o.col); }
};

enum class Lang { Plain, CLike, Python, Shell, Ruby, Json, Markup, Css, Markdown, Yaml, Toml, Sql, Lua, Go, Rust };
Lang lang_for_path(const std::string& path);
const char* lang_name(Lang l);

struct CodeDoc {
    std::vector<std::string> lines{""};
    CodePos cursor, anchor;
    float scroll_x = 0, scroll_y = 0;
    Lang lang = Lang::Plain;
    std::string eol = "\n";
    bool final_newline = true;
    bool use_tabs = false;
    int indent = 4;
    uint64_t version = 0;  // bumped on every edit

    // undo
    struct Snapshot { std::vector<std::string> lines; CodePos cursor, anchor; };
    std::vector<Snapshot> undo, redo;
    double last_edit_time = -10;
    int last_edit_kind = 0;

    // highlight cache: block-comment / string state at the start of each line
    uint64_t hl_version = ~0ull;
    std::vector<uint8_t> line_state;

    // transient
    bool selecting = false;
    double blink_t = 0;
    bool want_reveal = false;

    void set_text(const std::string& text, const std::string& path);
    std::string text() const;
    bool has_selection() const { return cursor != anchor; }
    std::pair<CodePos, CodePos> selection() const { return cursor < anchor ? std::make_pair(cursor, anchor) : std::make_pair(anchor, cursor); }
    std::string selected_text() const;
    void reveal() { want_reveal = true; }
};

// Returns true if the text changed this frame.
bool code_editor(Context& c, Id id, const Rect& r, CodeDoc& doc);

}  // namespace ui
