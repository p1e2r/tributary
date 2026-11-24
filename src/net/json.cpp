// SPDX-License-Identifier: MIT
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace json {

static const Value& null_value() {
    static Value v;
    return v;
}
static const Array& empty_array() {
    static Array a;
    return a;
}
static const Object& empty_object() {
    static Object o;
    return o;
}

const Value& Value::operator[](const std::string& k) const {
    if (!is_object()) return null_value();
    auto& o = *std::get<std::shared_ptr<Object>>(v);
    auto it = o.find(k);
    return it == o.end() ? null_value() : it->second;
}

const Value& Value::operator[](size_t i) const {
    if (!is_array()) return null_value();
    auto& a = *std::get<std::shared_ptr<Array>>(v);
    return i < a.size() ? a[i] : null_value();
}

Value& Value::set(const std::string& k, Value val) {
    if (!is_object()) v = std::make_shared<Object>();
    auto& o = *std::get<std::shared_ptr<Object>>(v);
    return o[k] = std::move(val);
}

void Value::push(Value val) {
    if (!is_array()) v = std::make_shared<Array>();
    std::get<std::shared_ptr<Array>>(v)->push_back(std::move(val));
}

std::string Value::str(const std::string& def) const {
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto d = std::get_if<double>(&v)) {
        char buf[32];
        if (std::floor(*d) == *d && std::fabs(*d) < 1e15) std::snprintf(buf, sizeof buf, "%lld", (long long)*d);
        else std::snprintf(buf, sizeof buf, "%g", *d);
        return buf;
    }
    return def;
}

double Value::num(double def) const {
    if (auto d = std::get_if<double>(&v)) return *d;
    return def;
}

bool Value::boolean(bool def) const {
    if (auto b = std::get_if<bool>(&v)) return *b;
    return def;
}

const Array& Value::arr() const { return is_array() ? *std::get<std::shared_ptr<Array>>(v) : empty_array(); }
const Object& Value::obj() const { return is_object() ? *std::get<std::shared_ptr<Object>>(v) : empty_object(); }
size_t Value::size() const { return is_array() ? arr().size() : is_object() ? obj().size() : 0; }

static void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else out += char(c);
        }
    }
    out += '"';
}

std::string Value::dump(bool pretty, int indent) const {
    std::string out;
    std::string pad = pretty ? std::string(size_t(indent + 2), ' ') : "";
    std::string pad_end = pretty ? std::string(size_t(indent), ' ') : "";
    if (is_null()) return "null";
    if (auto b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (std::holds_alternative<double>(v)) return str();
    if (auto s = std::get_if<std::string>(&v)) { dump_string(out, *s); return out; }
    if (is_array()) {
        auto& a = arr();
        if (a.empty()) return "[]";
        out += pretty ? "[\n" : "[";
        for (size_t i = 0; i < a.size(); i++) {
            out += pad + a[i].dump(pretty, indent + 2);
            if (i + 1 < a.size()) out += ",";
            if (pretty) out += "\n";
        }
        out += pad_end + "]";
        return out;
    }
    auto& o = obj();
    if (o.empty()) return "{}";
    out += pretty ? "{\n" : "{";
    size_t i = 0;
    for (auto& [k, val] : o) {
        out += pad;
        dump_string(out, k);
        out += pretty ? ": " : ":";
        out += val.dump(pretty, indent + 2);
        if (++i < o.size()) out += ",";
        if (pretty) out += "\n";
    }
    out += pad_end + "}";
    return out;
}

namespace {
struct Parser {
    const std::string& s;
    size_t p = 0;
    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON parse error: ") + what + " at offset " + std::to_string(p));
    }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++; }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s.compare(p, n, w) == 0) { p += n; return true; }
        return false;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    }
    uint32_t hex4() {
        if (p + 4 > s.size()) fail("bad \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }
    std::string string() {
        if (s[p] != '"') fail("expected string");
        p++;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) fail("bad escape");
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        if (p >= s.size()) fail("unterminated string");
        p++;
        return out;
    }
    Value value(int depth = 0) {
        if (depth > 200) fail("nesting too deep");
        ws();
        if (p >= s.size()) fail("unexpected end");
        char c = s[p];
        if (c == '{') {
            p++;
            Object o;
            ws();
            if (p < s.size() && s[p] == '}') { p++; return Value(std::move(o)); }
            for (;;) {
                ws();
                std::string k = string();
                ws();
                if (p >= s.size() || s[p] != ':') fail("expected ':'");
                p++;
                o[k] = value(depth + 1);
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; break; }
                fail("expected ',' or '}'");
            }
            return Value(std::move(o));
        }
        if (c == '[') {
            p++;
            Array a;
            ws();
            if (p < s.size() && s[p] == ']') { p++; return Value(std::move(a)); }
            for (;;) {
                a.push_back(value(depth + 1));
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; break; }
                fail("expected ',' or ']'");
            }
            return Value(std::move(a));
        }
        if (c == '"') return Value(string());
        if (lit("true")) return Value(true);
        if (lit("false")) return Value(false);
        if (lit("null")) return Value(nullptr);
        size_t start = p;
        if (s[p] == '-') p++;
        while (p < s.size() && ((s[p] >= '0' && s[p] <= '9') || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) p++;
        if (start == p) fail("unexpected character");
        return Value(std::strtod(s.c_str() + start, nullptr));
    }
};
}  // namespace

Value parse(const std::string& text) {
    Parser ps{text};
    Value v = ps.value();
    ps.ws();
    return v;
}

}  // namespace json
