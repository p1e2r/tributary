// SPDX-License-Identifier: MIT
#include "code_editor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace ui {

using gfx::utf8_next_boundary;
using gfx::utf8_prev_boundary;
using plat::Key;

// ================================================================ languages
static std::string lower_ext(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string lb;
    for (char ch : base) lb += char(std::tolower((unsigned char)ch));
    if (lb == "makefile" || lb == "dockerfile" || lb == ".gitignore" || lb == ".gitattributes" || lb == ".env") return "sh";
    if (lb == "cmakelists.txt") return "sh";
    auto dot = lb.find_last_of('.');
    return dot == std::string::npos ? "" : lb.substr(dot + 1);
}

Lang lang_for_path(const std::string& path) {
    std::string e = lower_ext(path);
    static const char* clike[] = {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "hxx", "inc", "m", "mm", "js", "mjs", "cjs", "jsx", "ts", "tsx",
                                  "java", "cs", "kt", "kts", "swift", "scala", "dart", "php", "groovy", "gradle", "zig", "glsl", "frag",
                                  "vert", "hlsl", "metal", "proto", "v", "sol"};
    for (auto* x : clike) if (e == x) return Lang::CLike;
    if (e == "go") return Lang::Go;
    if (e == "rs") return Lang::Rust;
    if (e == "py" || e == "pyw" || e == "pyi") return Lang::Python;
    if (e == "sh" || e == "bash" || e == "zsh" || e == "fish" || e == "ps1" || e == "bat" || e == "cmd" || e == "cmake") return Lang::Shell;
    if (e == "rb" || e == "rake" || e == "gemspec") return Lang::Ruby;
    if (e == "json" || e == "jsonc" || e == "json5" || e == "lock") return Lang::Json;
    if (e == "html" || e == "htm" || e == "xml" || e == "svg" || e == "vue" || e == "svelte" || e == "xaml" || e == "plist") return Lang::Markup;
    if (e == "css" || e == "scss" || e == "sass" || e == "less") return Lang::Css;
    if (e == "md" || e == "markdown" || e == "mdx" || e == "rst") return Lang::Markdown;
    if (e == "yml" || e == "yaml") return Lang::Yaml;
    if (e == "toml" || e == "ini" || e == "cfg" || e == "conf") return Lang::Toml;
    if (e == "sql") return Lang::Sql;
    if (e == "lua") return Lang::Lua;
    return Lang::Plain;
}

const char* lang_name(Lang l) {
    switch (l) {
        case Lang::CLike: return "C-like";
        case Lang::Python: return "Python";
        case Lang::Shell: return "Shell";
        case Lang::Ruby: return "Ruby";
        case Lang::Json: return "JSON";
        case Lang::Markup: return "HTML/XML";
        case Lang::Css: return "CSS";
        case Lang::Markdown: return "Markdown";
        case Lang::Yaml: return "YAML";
        case Lang::Toml: return "TOML";
        case Lang::Sql: return "SQL";
        case Lang::Lua: return "Lua";
        case Lang::Go: return "Go";
        case Lang::Rust: return "Rust";
        default: return "Plain text";
    }
}

static const char* line_comment(Lang l) {
    switch (l) {
        case Lang::CLike: case Lang::Go: case Lang::Rust: case Lang::Json: return "//";
        case Lang::Python: case Lang::Shell: case Lang::Ruby: case Lang::Yaml: case Lang::Toml: return "#";
        case Lang::Sql: case Lang::Lua: return "--";
        default: return nullptr;
    }
}

static bool is_keyword(Lang l, std::string_view w) {
    static const char* clike[] = {"if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
        "struct", "class", "union", "enum", "typedef", "namespace", "using", "template", "typename", "public", "private", "protected",
        "static", "const", "constexpr", "consteval", "inline", "virtual", "override", "final", "extern", "volatile", "mutable", "explicit",
        "friend", "operator", "new", "delete", "this", "sizeof", "alignof", "decltype", "auto", "void", "int", "char", "bool", "float",
        "double", "long", "short", "unsigned", "signed", "true", "false", "nullptr", "NULL", "try", "catch", "throw", "noexcept",
        "include", "define", "ifdef", "ifndef", "endif", "pragma", "function", "var", "let", "import", "export", "from", "as", "async",
        "await", "yield", "typeof", "instanceof", "in", "of", "interface", "type", "implements", "extends", "super", "null", "undefined",
        "package", "abstract", "synchronized", "throws", "finally", "boolean", "byte", "string", "object", "readonly", "declare", "module",
        "fun", "val", "when", "is", "func", "guard", "self", "Self", "init", "deinit", "where", "protocol", "extension", "fn", "match",
        "loop", "impl", "trait", "mut", "pub", "mod", "use", "crate", "ref", "move", "dyn", "unsafe", "go", "chan", "select", "defer",
        "range", "map", "nil", "lambda", "def", "elif", "pass", "with", "not", "and", "or", "None", "True", "False", "global", "nonlocal",
        "raise", "except", "assert", "del", "print", "then", "fi", "esac", "done", "local", "echo", "end", "begin", "unless", "until",
        "elsif", "require", "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "JOIN",
        "LEFT", "RIGHT", "INNER", "OUTER", "ON", "GROUP", "BY", "ORDER", "LIMIT", "AND", "OR", "NOT", "NULL", "AS", "DISTINCT", "PRIMARY",
        "KEY", "INDEX", "ALTER", "DROP", "local", "repeat", "elseif"};
    if (l == Lang::Plain || l == Lang::Markdown || l == Lang::Markup || l == Lang::Css) return false;
    if (l == Lang::Json) return w == "true" || w == "false" || w == "null";
    for (auto* k : clike) if (w == k) return true;
    return false;
}

enum Tok : uint8_t { TText, TKeyword, TString, TComment, TNumber, TFunction, TType, TTag, TAttr, THeading, TConflict };
struct Span { size_t a, b; Tok t; };

// state: 0 normal, 1 block comment, 2 python triple """ string, 3 python ''' string, 4 markup comment, 5 backtick template
static uint8_t tokenize(Lang lang, const std::string& s, uint8_t state, std::vector<Span>* out) {
    size_t i = 0, n = s.size();
    auto push = [&](size_t a, size_t b, Tok t) { if (out && b > a) out->push_back({a, b, t}); };
    const char* lc = line_comment(lang);
    // merge conflict markers stand out in every language
    if (n >= 7 && (s.compare(0, 7, "<<<<<<<") == 0 || s.compare(0, 7, "=======") == 0 || s.compare(0, 7, ">>>>>>>") == 0) &&
        (n == 7 || s[7] == ' ')) {
        push(0, n, TConflict);
        return state;
    }
    if (lang == Lang::Markdown) {
        size_t k = 0;
        while (k < n && s[k] == ' ') k++;
        if (k < n && s[k] == '#') { push(0, n, THeading); return 0; }
        if (s.compare(k, 3, "```") == 0) { push(0, n, TString); return state ^ 6; }
        if (state == 6) { push(0, n, TString); return 6; }
        // inline code spans; everything else is plain text, **bold** gets the keyword color
        size_t last = 0;
        for (size_t p = 0; p < n; p++) {
            if (s[p] == '`') {
                size_t q = s.find('`', p + 1);
                if (q == std::string::npos) break;
                push(last, p, TText);
                push(p, q + 1, TString);
                p = q;
                last = q + 1;
            } else if (s.compare(p, 2, "**") == 0) {
                size_t q = s.find("**", p + 2);
                if (q == std::string::npos) break;
                push(last, p, TText);
                push(p, q + 2, TKeyword);
                p = q + 1;
                last = q + 2;
            }
        }
        push(last, n, TText);
        return state;
    }
    while (i < n) {
        if (state == 1) {
            size_t e = s.find("*/", i);
            if (e == std::string::npos) { push(i, n, TComment); return 1; }
            push(i, e + 2, TComment);
            i = e + 2;
            state = 0;
            continue;
        }
        if (state == 2 || state == 3) {
            const char* q = state == 2 ? "\"\"\"" : "'''";
            size_t e = s.find(q, i);
            if (e == std::string::npos) { push(i, n, TString); return state; }
            push(i, e + 3, TString);
            i = e + 3;
            state = 0;
            continue;
        }
        if (state == 4) {
            size_t e = s.find("-->", i);
            if (e == std::string::npos) { push(i, n, TComment); return 4; }
            push(i, e + 3, TComment);
            i = e + 3;
            state = 0;
            continue;
        }
        if (state == 5) {
            size_t e = i;
            while (e < n && s[e] != '`') e += s[e] == '\\' ? 2 : 1;
            if (e >= n) { push(i, n, TString); return 5; }
            push(i, e + 1, TString);
            i = e + 1;
            state = 0;
            continue;
        }
        char ch = s[i];
        // comments
        if (lc && s.compare(i, std::strlen(lc), lc) == 0 && !(lang == Lang::Shell && i > 0 && s[i - 1] == '$')) { push(i, n, TComment); return 0; }
        if ((lang == Lang::CLike || lang == Lang::Go || lang == Lang::Rust || lang == Lang::Css || lang == Lang::Json) && s.compare(i, 2, "/*") == 0) {
            state = 1;
            continue;
        }
        if (lang == Lang::Markup && s.compare(i, 4, "<!--") == 0) { state = 4; continue; }
        if (lang == Lang::Python && (s.compare(i, 3, "\"\"\"") == 0 || s.compare(i, 3, "'''") == 0)) {
            state = s[i] == '"' ? 2 : 3;
            push(i, i + 3, TString);
            i += 3;
            continue;
        }
        if (lang == Lang::CLike && ch == '`') { push(i, i + 1, TString); i++; state = 5; continue; }
        // strings
        if (ch == '"' || (ch == '\'' && lang != Lang::Markdown && lang != Lang::Rust) || (ch == '`' && lang == Lang::Go)) {
            size_t e = i + 1;
            while (e < n && s[e] != ch) e += s[e] == '\\' ? 2 : 1;
            e = std::min(n, e + 1);
            // JSON object keys look like attributes
            bool key = false;
            if (lang == Lang::Json) {
                size_t k = e;
                while (k < n && s[k] == ' ') k++;
                key = k < n && s[k] == ':';
            }
            push(i, e, key ? TAttr : TString);
            i = e;
            continue;
        }
        // markup tags
        if (lang == Lang::Markup && ch == '<') {
            size_t e = i + 1;
            if (e < n && (s[e] == '/' || s[e] == '!' || s[e] == '?')) e++;
            size_t ns = e;
            while (e < n && (std::isalnum((unsigned char)s[e]) || s[e] == '-' || s[e] == ':' || s[e] == '.')) e++;
            push(i, ns, TText);
            push(ns, e, TTag);
            i = e;
            // attributes until '>'
            while (i < n && s[i] != '>') {
                if (s[i] == '"' || s[i] == '\'') {
                    char q = s[i];
                    size_t q2 = s.find(q, i + 1);
                    q2 = q2 == std::string::npos ? n : q2 + 1;
                    push(i, q2, TString);
                    i = q2;
                } else if (std::isalpha((unsigned char)s[i])) {
                    size_t a = i;
                    while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == ':' || s[i] == '@')) i++;
                    push(a, i, TAttr);
                } else i++;
            }
            continue;
        }
        // numbers
        if (std::isdigit((unsigned char)ch) && (i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            size_t e = i + 1;
            while (e < n && (std::isalnum((unsigned char)s[e]) || s[e] == '.' || s[e] == '_')) e++;
            push(i, e, TNumber);
            i = e;
            continue;
        }
        // identifiers
        if (std::isalpha((unsigned char)ch) || ch == '_' || ch == '$' || ch == '@' || (ch == '#' && (lang == Lang::CLike))) {
            size_t e = i + 1;
            while (e < n && (std::isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '$' || s[e] == '-' && lang == Lang::Css)) e++;
            std::string_view w(s.data() + i, e - i);
            std::string_view bare = (w[0] == '#' || w[0] == '@') ? w.substr(1) : w;
            Tok t = TText;
            if (lang == Lang::Css) {
                size_t k = e;
                while (k < n && s[k] == ' ') k++;
                t = (k < n && s[k] == ':') ? TAttr : TText;
                if (w[0] == '@') t = TKeyword;
            } else if (lang == Lang::Yaml || lang == Lang::Toml) {
                size_t k = e;
                while (k < n && s[k] == ' ') k++;
                if (k < n && (s[k] == ':' || s[k] == '=')) t = TAttr;
                else if (is_keyword(lang, w)) t = TKeyword;
            } else if (is_keyword(lang, bare)) t = TKeyword;
            else {
                size_t k = e;
                while (k < n && s[k] == ' ') k++;
                if (k < n && s[k] == '(' ) t = TFunction;
                else if (std::isupper((unsigned char)bare.empty() ? 'a' : bare[0]) && lang != Lang::Plain) t = TType;
            }
            push(i, e, t);
            i = e;
            continue;
        }
        size_t e = utf8_next_boundary(s, i);
        push(i, e, TText);
        i = e;
    }
    return state == 6 ? 6 : state;
}

static Color tok_color(const Theme& T, Tok t) {
    // Primer syntax colors (github.com's light and dark code views)
    bool d = T.dark;
    switch (t) {
        case TKeyword: return Color::hex(d ? 0xff7b72 : 0xcf222e);
        case TString: return Color::hex(d ? 0xa5d6ff : 0x0a3069);
        case TComment: return Color::hex(d ? 0x8b949e : 0x6e7781);
        case TNumber: return Color::hex(d ? 0x79c0ff : 0x0550ae);
        case TFunction: return Color::hex(d ? 0xd2a8ff : 0x8250df);
        case TType: return Color::hex(d ? 0xffa657 : 0x953800);
        case TTag: return Color::hex(d ? 0x7ee787 : 0x116329);
        case TAttr: return Color::hex(d ? 0x79c0ff : 0x0550ae);
        case THeading: return Color::hex(d ? 0x79c0ff : 0x0550ae);
        case TConflict: return Color::hex(d ? 0xf0883e : 0xbc4c00);
        default: return T.text;
    }
}

// ================================================================ document
void CodeDoc::set_text(const std::string& text, const std::string& path) {
    lines.clear();
    eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    size_t p = 0;
    while (true) {
        size_t nl = text.find('\n', p);
        std::string l = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    final_newline = !text.empty() && text.back() == '\n';
    if (final_newline && lines.size() > 1 && lines.back().empty()) lines.pop_back();
    if (lines.empty()) lines.push_back("");
    lang = lang_for_path(path);
    // indentation style
    int tabs = 0, spaces2 = 0, spaces4 = 0;
    for (auto& l : lines) {
        if (l.empty()) continue;
        if (l[0] == '\t') tabs++;
        else if (l.compare(0, 4, "    ") == 0) spaces4++;
        else if (l.compare(0, 2, "  ") == 0) spaces2++;
    }
    use_tabs = tabs > spaces2 + spaces4;
    indent = use_tabs ? 4 : (spaces2 > spaces4 ? 2 : 4);
    if (lang == Lang::Yaml || lang == Lang::Json || lang == Lang::Markup) indent = spaces4 > spaces2 ? 4 : 2;
    cursor = anchor = {};
    scroll_x = scroll_y = 0;
    undo.clear();
    redo.clear();
    version++;
}

std::string CodeDoc::text() const {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        out += lines[i];
        if (i + 1 < lines.size() || final_newline) out += eol;
    }
    return out;
}

std::string CodeDoc::selected_text() const {
    auto [a, b] = selection();
    if (a.line == b.line) return lines[a.line].substr(a.col, b.col - a.col);
    std::string out = lines[a.line].substr(a.col) + "\n";
    for (size_t l = a.line + 1; l < b.line; l++) out += lines[l] + "\n";
    out += lines[b.line].substr(0, b.col);
    return out;
}

namespace {

bool is_word(unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch >= 0x80; }

void clamp(CodeDoc& d, CodePos& p) {
    p.line = std::min(p.line, d.lines.size() - 1);
    p.col = std::min(p.col, d.lines[p.line].size());
}

void snapshot(CodeDoc& d, int kind, double now) {
    bool coalesce = kind != 3 && kind == d.last_edit_kind && now - d.last_edit_time < 1.0 && !d.undo.empty();
    if (!coalesce) {
        d.undo.push_back({d.lines, d.cursor, d.anchor});
        if (d.undo.size() > 500) d.undo.erase(d.undo.begin());
    }
    d.redo.clear();
    d.last_edit_kind = kind;
    d.last_edit_time = now;
}

CodePos erase(CodeDoc& d, CodePos a, CodePos b) {
    if (b < a) std::swap(a, b);
    if (a.line == b.line) d.lines[a.line].erase(a.col, b.col - a.col);
    else {
        d.lines[a.line] = d.lines[a.line].substr(0, a.col) + d.lines[b.line].substr(b.col);
        d.lines.erase(d.lines.begin() + long(a.line) + 1, d.lines.begin() + long(b.line) + 1);
    }
    d.version++;
    return a;
}

CodePos insert(CodeDoc& d, CodePos p, const std::string& text) {
    std::string t;
    t.reserve(text.size());
    for (char ch : text) if (ch != '\r') t += ch;
    std::string tail = d.lines[p.line].substr(p.col);
    d.lines[p.line].resize(p.col);
    size_t start = 0;
    size_t line = p.line;
    while (true) {
        size_t nl = t.find('\n', start);
        std::string chunk = t.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        d.lines[line] += chunk;
        if (nl == std::string::npos) break;
        d.lines.insert(d.lines.begin() + long(line) + 1, std::string());
        line++;
        start = nl + 1;
    }
    CodePos end{line, d.lines[line].size()};
    d.lines[line] += tail;
    d.version++;
    return end;
}

void replace_selection(CodeDoc& d, const std::string& text) {
    CodePos p = d.has_selection() ? erase(d, d.cursor, d.anchor) : d.cursor;
    d.cursor = d.anchor = insert(d, p, text);
}

std::string indent_unit(const CodeDoc& d) { return d.use_tabs ? "\t" : std::string(size_t(d.indent), ' '); }

std::string leading_ws(const std::string& l) {
    size_t k = 0;
    while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
    return l.substr(0, k);
}

size_t visual_col(const std::string& l, size_t col, int tab) {
    size_t v = 0;
    for (size_t i = 0; i < col && i < l.size();) {
        if (l[i] == '\t') { v = (v / size_t(tab) + 1) * size_t(tab); i++; }
        else { v++; i = utf8_next_boundary(l, i); }
    }
    return v;
}

size_t col_from_visual(const std::string& l, float vx, int tab) {
    size_t v = 0, i = 0;
    while (i < l.size()) {
        size_t nv = l[i] == '\t' ? (v / size_t(tab) + 1) * size_t(tab) : v + 1;
        if (vx < float(v) + float(nv - v) / 2.f) return i;
        v = nv;
        i = l[i] == '\t' ? i + 1 : utf8_next_boundary(l, i);
    }
    return l.size();
}

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

}  // namespace

// ================================================================ widget
bool code_editor(Context& c, Id id, const Rect& r, CodeDoc& d) {
    auto& T = c.theme;
    const float fs = 12, lh = 18;
    const float cw = c.text_w(FontId::Mono, fs, "M");
    const int tab = 4;  // display width of a tab character
    bool changed = false;
    c.register_focusable(id);
    c.capture_tab(id);
    bool focused = c.has_focus(id);
    clamp(d, d.cursor);
    clamp(d, d.anchor);

    int digits = 1;
    for (size_t n = d.lines.size(); n >= 10; n /= 10) digits++;
    float gutter = std::max(3, digits) * cw + 28;
    Rect text_r{r.x + gutter, r.y, r.w - gutter, r.h};
    float pad_top = 6;

    auto pos_at = [&](float mx, float my) {
        float rel = my - r.y - pad_top + d.scroll_y;
        long line = long(std::floor(rel / lh));
        line = std::clamp(line, 0L, long(d.lines.size()) - 1);
        float vx = (mx - text_r.x - 6 + d.scroll_x) / cw;
        return CodePos{size_t(line), col_from_visual(d.lines[size_t(line)], vx, tab)};
    };

    // ---------------- mouse
    bool hov = c.hovered(r);
    if (hov) c.set_cursor(c.mx < r.x + gutter ? plat::Cursor::Arrow : plat::Cursor::IBeam);
    if (hov && c.mouse_pressed[0]) {
        c.set_focus(id);
        focused = true;
        c.set_active(id);
        CodePos p = pos_at(c.mx, c.my);
        if (c.mx < text_r.x) {  // gutter click selects the line
            d.anchor = {p.line, 0};
            d.cursor = p.line + 1 < d.lines.size() ? CodePos{p.line + 1, 0} : CodePos{p.line, d.lines[p.line].size()};
        } else if (c.clicks >= 3) {
            d.anchor = {p.line, 0};
            d.cursor = {p.line, d.lines[p.line].size()};
        } else if (c.clicks == 2) {
            auto& l = d.lines[p.line];
            d.anchor = {p.line, word_left(l, p.col)};
            d.cursor = {p.line, word_right(l, p.col)};
        } else {
            d.cursor = p;
            if (!(c.mods & plat::ModShift)) d.anchor = p;
        }
        d.selecting = c.clicks == 1 && c.mx >= text_r.x;
        d.blink_t = c.time;
    } else if (c.mouse_pressed[0] && focused && !hov) {
        c.focus = 0;
        focused = false;
    }
    if (c.is_active(id) && c.mouse_down[0] && d.selecting) {
        d.cursor = pos_at(c.mx, c.my);
        if (c.my < r.y + 4) d.scroll_y -= lh;
        if (c.my > r.bottom() - 4) d.scroll_y += lh;
        c.request_redraw();
    }
    if (c.is_active(id) && c.mouse_released[0]) { c.clear_active(); d.selecting = false; }
    if (hov && (c.scroll_y != 0 || c.scroll_x != 0)) {
        if (c.mods & plat::ModShift) d.scroll_x -= c.scroll_y != 0 ? c.scroll_y : c.scroll_x;
        else { d.scroll_y -= c.scroll_y; d.scroll_x -= c.scroll_x; }
        c.scroll_x = c.scroll_y = 0;
    }

    // ---------------- keyboard
    if (focused) {
#ifdef __APPLE__
        const uint8_t WORD = plat::ModAlt;
        const bool mac = true;
#else
        const uint8_t WORD = plat::ModCtrl;
        const bool mac = false;
#endif
        const uint8_t CMD = plat::ModCmd;
        auto keys = c.keys;
        for (auto& k : keys) {
            uint8_t m = k.mods;
            bool shift = m & plat::ModShift;
            uint8_t base = uint8_t(m & ~plat::ModShift);
            bool handled = true;
            auto move = [&](CodePos p) {
                d.cursor = p;
                clamp(d, d.cursor);
                if (!shift) d.anchor = d.cursor;
                d.reveal();
            };
            auto& cur = d.cursor;
            const std::string& cl = d.lines[cur.line];
            switch (k.key) {
                case Key::Left:
                    if (!shift && d.has_selection() && base == 0) { move(d.selection().first); break; }
                    if (base == WORD) move({cur.line, word_left(cl, cur.col)});
                    else if (mac && base == CMD) move({cur.line, leading_ws(cl).size() == cur.col ? 0 : leading_ws(cl).size()});
                    else if (base == 0) move(cur.col > 0 ? CodePos{cur.line, utf8_prev_boundary(cl, cur.col)}
                                                          : cur.line > 0 ? CodePos{cur.line - 1, d.lines[cur.line - 1].size()} : cur);
                    else handled = false;
                    break;
                case Key::Right:
                    if (!shift && d.has_selection() && base == 0) { move(d.selection().second); break; }
                    if (base == WORD) move({cur.line, word_right(cl, cur.col)});
                    else if (mac && base == CMD) move({cur.line, cl.size()});
                    else if (base == 0) move(cur.col < cl.size() ? CodePos{cur.line, utf8_next_boundary(cl, cur.col)}
                                                                  : cur.line + 1 < d.lines.size() ? CodePos{cur.line + 1, 0} : cur);
                    else handled = false;
                    break;
                case Key::Up:
                case Key::Down: {
                    bool up = k.key == Key::Up;
                    if (base == plat::ModAlt) {
                        // move line(s) up/down
                        auto [a, b] = d.selection();
                        size_t l0 = a.line, l1 = b.line - (b.col == 0 && b.line > a.line ? 1 : 0);
                        if ((up && l0 == 0) || (!up && l1 + 1 >= d.lines.size())) break;
                        snapshot(d, 3, c.time);
                        if (up) { std::rotate(d.lines.begin() + long(l0) - 1, d.lines.begin() + long(l0), d.lines.begin() + long(l1) + 1); d.cursor.line--; d.anchor.line--; }
                        else { std::rotate(d.lines.begin() + long(l0), d.lines.begin() + long(l1) + 1, d.lines.begin() + long(l1) + 2); d.cursor.line++; d.anchor.line++; }
                        d.version++;
                        changed = true;
                        d.reveal();
                        break;
                    }
                    if (mac && base == CMD) { move(up ? CodePos{0, 0} : CodePos{d.lines.size() - 1, d.lines.back().size()}); break; }
                    if (base != 0) { handled = false; break; }
                    size_t vc = visual_col(cl, cur.col, tab);
                    if (up && cur.line == 0) { move({0, 0}); break; }
                    if (!up && cur.line + 1 >= d.lines.size()) { move({cur.line, cl.size()}); break; }
                    size_t nl = up ? cur.line - 1 : cur.line + 1;
                    move({nl, col_from_visual(d.lines[nl], float(vc), tab)});
                    break;
                }
                case Key::Home:
                    if (base == plat::ModCtrl || base == CMD) move({0, 0});
                    else { size_t ws = leading_ws(cl).size(); move({cur.line, cur.col == ws ? 0 : ws}); }
                    break;
                case Key::End:
                    if (base == plat::ModCtrl || base == CMD) move({d.lines.size() - 1, d.lines.back().size()});
                    else move({cur.line, cl.size()});
                    break;
                case Key::PageUp:
                case Key::PageDown: {
                    long page = long(r.h / lh) - 2;
                    long nl = long(cur.line) + (k.key == Key::PageUp ? -page : page);
                    nl = std::clamp(nl, 0L, long(d.lines.size()) - 1);
                    d.scroll_y += float(nl - long(cur.line)) * lh;
                    move({size_t(nl), std::min(cur.col, d.lines[size_t(nl)].size())});
                    break;
                }
                case Key::A:
                    if (base == CMD) { d.anchor = {0, 0}; d.cursor = {d.lines.size() - 1, d.lines.back().size()}; }
                    else handled = false;
                    break;
                case Key::C:
                case Key::X:
                    if (base != CMD) { handled = false; break; }
                    {
                        bool whole_line = !d.has_selection();
                        std::string t = whole_line ? cl + "\n" : d.selected_text();
                        c.set_clipboard(t);
                        if (k.key == Key::X) {
                            snapshot(d, 3, c.time);
                            if (whole_line) {
                                if (d.lines.size() > 1) d.lines.erase(d.lines.begin() + long(cur.line));
                                else d.lines[0].clear();
                                d.cursor = d.anchor = {std::min(cur.line, d.lines.size() - 1), 0};
                                d.version++;
                            } else replace_selection(d, "");
                            changed = true;
                            d.reveal();
                        }
                    }
                    break;
                case Key::V:
                    if (base != CMD) { handled = false; break; }
                    {
                        std::string t = c.clipboard();
                        if (!t.empty()) {
                            snapshot(d, 3, c.time);
                            replace_selection(d, t);
                            changed = true;
                            d.reveal();
                        }
                    }
                    break;
                case Key::Z:
                case Key::Y: {
                    bool redo = (k.key == Key::Z && base == CMD && shift) || (k.key == Key::Y && base == CMD && !shift);
                    bool undo = k.key == Key::Z && base == CMD && !shift;
                    if (undo && !d.undo.empty()) {
                        d.redo.push_back({d.lines, d.cursor, d.anchor});
                        auto s = std::move(d.undo.back());
                        d.undo.pop_back();
                        d.lines = std::move(s.lines);
                        d.cursor = s.cursor;
                        d.anchor = s.anchor;
                        d.version++;
                        d.last_edit_kind = 0;
                        changed = true;
                        d.reveal();
                    } else if (redo && !d.redo.empty()) {
                        d.undo.push_back({d.lines, d.cursor, d.anchor});
                        auto s = std::move(d.redo.back());
                        d.redo.pop_back();
                        d.lines = std::move(s.lines);
                        d.cursor = s.cursor;
                        d.anchor = s.anchor;
                        d.version++;
                        d.last_edit_kind = 0;
                        changed = true;
                        d.reveal();
                    } else if (!undo && !redo) handled = false;
                    break;
                }
                case Key::Slash: {
                    if (base != CMD) { handled = false; break; }
                    const char* lcm = line_comment(d.lang);
                    if (!lcm) break;
                    std::string pre = std::string(lcm) + " ";
                    auto [a, b] = d.selection();
                    size_t l0 = a.line, l1 = b.line - (b.col == 0 && b.line > a.line ? 1 : 0);
                    bool all = true;
                    size_t min_ws = std::string::npos;
                    for (size_t l = l0; l <= l1; l++) {
                        auto& s = d.lines[l];
                        size_t ws = leading_ws(s).size();
                        if (ws == s.size()) continue;
                        min_ws = std::min(min_ws, ws);
                        if (s.compare(ws, std::strlen(lcm), lcm) != 0) all = false;
                    }
                    if (min_ws == std::string::npos) break;
                    snapshot(d, 3, c.time);
                    for (size_t l = l0; l <= l1; l++) {
                        auto& s = d.lines[l];
                        size_t ws = leading_ws(s).size();
                        if (ws == s.size()) continue;
                        if (all) {
                            size_t cut = std::strlen(lcm) + (s.compare(ws, pre.size(), pre) == 0 ? 1 : 0);
                            s.erase(ws, cut);
                        } else s.insert(min_ws, pre);
                    }
                    d.version++;
                    clamp(d, d.cursor);
                    clamp(d, d.anchor);
                    changed = true;
                    break;
                }
                case Key::Enter: {
                    if (base != 0) { handled = false; break; }
                    snapshot(d, 3, c.time);
                    auto [a, b] = d.selection();
                    std::string before = d.lines[a.line].substr(0, a.col);
                    std::string ws = leading_ws(d.lines[a.line]);
                    if (ws.size() > a.col) ws = ws.substr(0, a.col);
                    std::string t = before;
                    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
                    char last = t.empty() ? 0 : t.back();
                    bool open = last == '{' || last == '[' || last == '(' ||
                                (last == ':' && (d.lang == Lang::Python || d.lang == Lang::Yaml));
                    std::string after = b.col < d.lines[b.line].size() ? d.lines[b.line].substr(b.col) : "";
                    size_t k2 = 0;
                    while (k2 < after.size() && after[k2] == ' ') k2++;
                    char next = k2 < after.size() ? after[k2] : 0;
                    bool closing = (last == '{' && next == '}') || (last == '[' && next == ']') || (last == '(' && next == ')');
                    std::string ins = "\n" + ws + (open ? indent_unit(d) : "");
                    replace_selection(d, ins);
                    if (closing) {
                        CodePos keep = d.cursor;
                        insert(d, d.cursor, "\n" + ws);
                        d.cursor = d.anchor = keep;
                    }
                    changed = true;
                    d.reveal();
                    break;
                }
                case Key::Tab: {
                    if (base != 0) { handled = false; break; }
                    snapshot(d, 3, c.time);
                    auto [a, b] = d.selection();
                    if (a.line != b.line || shift) {
                        size_t l1 = b.line - (b.col == 0 && b.line > a.line ? 1 : 0);
                        for (size_t l = a.line; l <= l1; l++) {
                            auto& s = d.lines[l];
                            if (shift) {
                                size_t rm = 0;
                                if (!s.empty() && s[0] == '\t') rm = 1;
                                else while (rm < size_t(d.indent) && rm < s.size() && s[rm] == ' ') rm++;
                                s.erase(0, rm);
                                if (d.cursor.line == l) d.cursor.col -= std::min(d.cursor.col, rm);
                                if (d.anchor.line == l) d.anchor.col -= std::min(d.anchor.col, rm);
                            } else if (!s.empty()) {
                                s.insert(0, indent_unit(d));
                                if (d.cursor.line == l) d.cursor.col += indent_unit(d).size();
                                if (d.anchor.line == l) d.anchor.col += indent_unit(d).size();
                            }
                        }
                        d.version++;
                    } else if (d.use_tabs) replace_selection(d, "\t");
                    else {
                        size_t vc = visual_col(d.lines[a.line], a.col, tab);
                        replace_selection(d, std::string(size_t(d.indent) - vc % size_t(d.indent), ' '));
                    }
                    changed = true;
                    d.reveal();
                    break;
                }
                case Key::Backspace:
                case Key::Delete: {
                    bool back = k.key == Key::Backspace;
                    if (base != 0 && base != WORD && !(mac && base == CMD)) { handled = false; break; }
                    snapshot(d, 2, c.time);
                    if (d.has_selection()) replace_selection(d, "");
                    else if (back) {
                        CodePos from = cur;
                        if (base == WORD) from = {cur.line, word_left(cl, cur.col)};
                        else if (mac && base == CMD) from = {cur.line, 0};
                        else if (cur.col == 0) { if (cur.line == 0) break; from = {cur.line - 1, d.lines[cur.line - 1].size()}; }
                        else {
                            std::string ws = leading_ws(cl);
                            if (!d.use_tabs && cur.col <= ws.size() && cur.col > 0 && ws.find('\t') == std::string::npos) {
                                size_t rm = cur.col % size_t(d.indent);
                                if (rm == 0) rm = size_t(d.indent);
                                from = {cur.line, cur.col - std::min(rm, cur.col)};
                            } else from = {cur.line, utf8_prev_boundary(cl, cur.col)};
                        }
                        d.cursor = d.anchor = erase(d, from, cur);
                    } else {
                        CodePos to = cur;
                        if (base == WORD) to = {cur.line, word_right(cl, cur.col)};
                        else if (cur.col >= cl.size()) { if (cur.line + 1 >= d.lines.size()) break; to = {cur.line + 1, 0}; }
                        else to = {cur.line, utf8_next_boundary(cl, cur.col)};
                        d.cursor = d.anchor = erase(d, cur, to);
                    }
                    changed = true;
                    d.reveal();
                    break;
                }
                case Key::Escape:
                    if (d.has_selection()) d.anchor = d.cursor;
                    else handled = false;
                    break;
                default: handled = false;
            }
            if (handled) { c.take_key(k.key, k.mods); d.blink_t = c.time; }
        }
        // typed text
        if (!c.text_input.empty()) {
            std::string in;
            for (unsigned char ch : c.text_input) if (ch >= 0x20 || ch >= 0x80) in += char(ch);
            c.text_input.clear();
            if (!in.empty()) {
                snapshot(d, 1, c.time);
                // closing bracket on a whitespace-only line dedents it
                if ((in == "}" || in == "]" || in == ")") && !d.has_selection()) {
                    auto& l = d.lines[d.cursor.line];
                    if (leading_ws(l).size() == l.size() && l.size() == d.cursor.col && !l.empty()) {
                        size_t rm = l[0] == '\t' ? 1 : std::min(l.size(), size_t(d.indent));
                        l.erase(0, rm);
                        d.cursor.col = d.anchor.col = l.size();
                    }
                }
                replace_selection(d, in);
                changed = true;
                d.reveal();
                d.blink_t = c.time;
            }
        }
    }

    // ---------------- scrolling limits / reveal
    float content_h = float(d.lines.size()) * lh + pad_top + r.h * 0.5f;
    size_t widest = 0;
    {
        // estimate width from visible lines only (cheap)
        size_t first = size_t(std::max(0.f, d.scroll_y / lh));
        for (size_t l = first; l < d.lines.size() && l < first + size_t(r.h / lh) + 2; l++) widest = std::max(widest, visual_col(d.lines[l], d.lines[l].size(), tab));
    }
    if (d.want_reveal) {
        d.want_reveal = false;
        float cy = float(d.cursor.line) * lh;
        if (cy < d.scroll_y) d.scroll_y = cy;
        if (cy + lh > d.scroll_y + r.h - pad_top - 4) d.scroll_y = cy + lh - r.h + pad_top + 4;
        float cx = float(visual_col(d.lines[d.cursor.line], d.cursor.col, tab)) * cw;
        if (cx < d.scroll_x) d.scroll_x = std::max(0.f, cx - 40);
        if (cx > d.scroll_x + text_r.w - 30) d.scroll_x = cx - text_r.w + 60;
        widest = std::max(widest, size_t(cx / cw) + 1);
    }
    d.scroll_y = std::clamp(d.scroll_y, 0.f, std::max(0.f, content_h - r.h));
    d.scroll_x = std::clamp(d.scroll_x, 0.f, std::max(d.scroll_x > 0 ? d.scroll_x : 0.f, float(widest) * cw - text_r.w + 60));
    d.scroll_x = std::max(0.f, d.scroll_x);

    // ---------------- highlight states
    if (d.hl_version != d.version || d.line_state.size() != d.lines.size() + 1) {
        d.line_state.resize(d.lines.size() + 1);
        uint8_t st = 0;
        for (size_t l = 0; l < d.lines.size(); l++) {
            d.line_state[l] = st;
            st = tokenize(d.lang, d.lines[l], st, nullptr);
        }
        d.line_state[d.lines.size()] = st;
        d.hl_version = d.version;
    }

    // ---------------- draw
    c.r.rect(r, T.bg);
    c.r.push_clip(r);
    size_t first = size_t(std::max(0.f, (d.scroll_y - pad_top) / lh));
    size_t last = std::min(d.lines.size(), first + size_t(r.h / lh) + 2);
    auto [sa, sb] = d.selection();
    float base_x = text_r.x + 6 - d.scroll_x;
    std::vector<Span> spans;
    // current line highlight
    if (!d.has_selection() && focused) {
        float y = r.y + pad_top + float(d.cursor.line) * lh - d.scroll_y;
        c.r.rect({r.x, y, r.w, lh}, T.dark ? Color::hex(0xffffff, 0.04f) : Color::hex(0x0366d6, 0.04f));
    }
    for (size_t l = first; l < last; l++) {
        float y = r.y + pad_top + float(l) * lh - d.scroll_y;
        const std::string& s = d.lines[l];
        // selection
        if (d.has_selection() && l >= sa.line && l <= sb.line) {
            size_t c0 = l == sa.line ? sa.col : 0, c1 = l == sb.line ? sb.col : s.size();
            float x0 = base_x + float(visual_col(s, c0, tab)) * cw, x1 = base_x + float(visual_col(s, c1, tab)) * cw;
            if (l != sb.line) x1 += cw * 0.6f;
            c.r.push_clip(text_r);
            c.r.rect({x0, y, x1 - x0, lh}, T.accent_emphasis.with_alpha(focused ? 0.25f : 0.12f));
            c.r.pop_clip();
        }
        // tokens
        spans.clear();
        tokenize(d.lang, s, d.line_state[l], &spans);
        if (!spans.empty() && spans[0].t == TConflict) c.r.rect({text_r.x, y, text_r.w, lh}, T.attention_subtle);
        c.r.push_clip(text_r);
        for (auto& sp : spans) {
            float x = base_x + float(visual_col(s, sp.a, tab)) * cw;
            if (x > text_r.right()) break;
            std::string seg = s.substr(sp.a, sp.b - sp.a);
            if (seg.find('\t') != std::string::npos) {
                // draw tab-containing text piecewise at its visual columns
                size_t p = 0;
                while (p < seg.size()) {
                    size_t q = seg.find('\t', p);
                    std::string part = seg.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    float px = base_x + float(visual_col(s, sp.a + p, tab)) * cw;
                    if (!part.empty()) c.r.draw_text(FontId::Mono, fs, px, y + 2, part, tok_color(T, sp.t));
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
            } else if (x + float(seg.size()) * cw >= text_r.x) {
                c.r.draw_text(FontId::Mono, fs, x, y + 2, seg, tok_color(T, sp.t));
            }
        }
        c.r.pop_clip();
        // gutter
        bool cur_line = l == d.cursor.line;
        c.r.draw_text_in(FontId::Mono, fs, {r.x, y, gutter - 16, lh}, std::to_string(l + 1), cur_line ? T.text : T.diff_line_number, Align::Right);
    }
    c.r.line_v(text_r.x - 1, r.y, r.bottom(), T.border);
    // caret
    if (focused) {
        float y = r.y + pad_top + float(d.cursor.line) * lh - d.scroll_y;
        float x = base_x + float(visual_col(d.lines[d.cursor.line], d.cursor.col, tab)) * cw;
        bool on = std::fmod(c.time - d.blink_t, 1.0) < 0.55;
        if (on && x >= text_r.x - 1 && x <= text_r.right()) c.r.rect({x - 1, y + 1, 2, lh - 2}, T.accent_emphasis);
        c.animate();
        c.win.set_text_input_rect(x, y, 1, lh);
    }
    // scroll indicator
    float total = float(d.lines.size()) * lh + pad_top;
    if (total > r.h) {
        float th = std::max(24.f, r.h * r.h / content_h);
        float ty = r.y + (r.h - th) * (d.scroll_y / std::max(1.f, content_h - r.h));
        c.r.rect({r.right() - 8, ty, 6, th}, T.scrollbar, 3);
        // cursor marker in the scroll track, like most editors
        float my = r.y + r.h * float(d.cursor.line) / float(std::max<size_t>(1, d.lines.size()));
        c.r.rect({r.right() - 10, my, 10, 2}, T.accent_emphasis.with_alpha(0.7f));
    }
    c.r.pop_clip();
    return changed;
}

}  // namespace ui
