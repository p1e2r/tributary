// SPDX-License-Identifier: MIT
#include "refs.hpp"

#include <algorithm>
#include <cstdlib>

namespace git {

// ================================================================ names
bool valid_ref_name(std::string_view n) {
    if (n.empty() || n.back() == '/' || n.back() == '.' || n.front() == '/' || n.front() == '-') return false;
    if (n.find("..") != std::string_view::npos || n.find("@{") != std::string_view::npos) return false;
    if (n.find("//") != std::string_view::npos) return false;
    if (n.size() >= 5 && n.substr(n.size() - 5) == ".lock") return false;
    if (n == "@") return false;
    for (unsigned char c : n)
        if (c < 32 || c == 127 || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' ||
            c == '[' || c == '\\')
            return false;
    for (size_t i = 0; i < n.size(); i++)
        if (n[i] == '.' && (i == 0 || n[i - 1] == '/')) return false;
    return true;
}

std::string shorten_ref(const std::string& full) {
    for (const char* p : {"refs/heads/", "refs/remotes/", "refs/tags/", "refs/"})
        if (starts_with(full, p)) return full.substr(std::strlen(p));
    return full;
}

// ================================================================ RefDB
std::map<std::string, Ref> RefDB::packed() const {
    std::map<std::string, Ref> out;
    auto data = try_read_file(gitdir_ / "packed-refs");
    if (!data) return out;
    Ref* last = nullptr;
    for (auto line : split_lines(*data)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '^') {
            if (last && line.size() >= 41) last->peeled = ObjectId::try_hex(line.substr(1, 40));
            continue;
        }
        if (line.size() < 42) continue;
        auto id = ObjectId::try_hex(line.substr(0, 40));
        if (!id) continue;
        Ref r;
        r.name = std::string(line.substr(41));
        if (!r.name.empty() && r.name.back() == '\r') r.name.pop_back();
        r.target = *id;
        last = &(out[r.name] = r);
    }
    return out;
}

void RefDB::write_packed(const std::map<std::string, Ref>& refs) {
    std::string s = "# pack-refs with: peeled fully-peeled sorted \n";
    for (auto& [name, r] : refs) {
        s += r.target.hex() + " " + name + "\n";
        if (r.peeled) s += "^" + r.peeled->hex() + "\n";
    }
    LockFile lf(gitdir_ / "packed-refs");
    lf.write(s);
    lf.commit();
}

std::optional<std::string> RefDB::read_raw(const std::string& name) const {
    if (auto d = try_read_file(gitdir_ / git_to_path(name))) {
        auto t = trim(*d);
        if (!t.empty()) return t;
    }
    auto p = packed();
    auto it = p.find(name);
    if (it != p.end()) return it->second.target.hex();
    return std::nullopt;
}

std::optional<ObjectId> RefDB::resolve(const std::string& name) const {
    std::string cur = name;
    for (int depth = 0; depth < 10; depth++) {
        auto raw = read_raw(cur);
        if (!raw) return std::nullopt;
        if (starts_with(*raw, "ref:")) {
            cur = trim(raw->substr(4));
            continue;
        }
        return ObjectId::try_hex(raw->substr(0, 40));
    }
    return std::nullopt;
}

std::string RefDB::head_target() const {
    auto raw = read_raw("HEAD");
    if (raw && starts_with(*raw, "ref:")) return trim(raw->substr(4));
    return "";
}

std::vector<Ref> RefDB::list(const std::string& prefix) const {
    std::map<std::string, Ref> all = packed();
    for (auto it = all.begin(); it != all.end();)
        it = starts_with(it->first, prefix) ? std::next(it) : all.erase(it);
    std::error_code ec;
    auto root = gitdir_ / git_to_path(prefix);
    if (fs::is_directory(root, ec)) {
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            auto rel = path_to_git(fs::relative(it->path(), gitdir_));
            if (rel.size() > 5 && rel.substr(rel.size() - 5) == ".lock") continue;
            auto d = try_read_file(it->path());
            if (!d) continue;
            auto t = trim(*d);
            Ref r;
            r.name = rel;
            if (starts_with(t, "ref:")) {
                r.symbolic = trim(t.substr(4));
                auto id = resolve(r.symbolic);
                if (!id) continue;
                r.target = *id;
            } else if (auto id = ObjectId::try_hex(t.substr(0, std::min<size_t>(40, t.size())))) {
                r.target = *id;
            } else {
                continue;
            }
            all[rel] = r;
        }
    }
    std::vector<Ref> out;
    for (auto& [k, v] : all) out.push_back(v);
    return out;
}

void RefDB::update(const std::string& name, const ObjectId& id, const std::string& msg,
                   const Signature& who, std::optional<ObjectId> expected_old) {
    std::string target = name;
    // writing through a symbolic ref (HEAD -> refs/heads/main)
    for (int i = 0; i < 10; i++) {
        auto raw = try_read_file(gitdir_ / git_to_path(target));
        if (raw && starts_with(trim(*raw), "ref:")) target = trim(trim(*raw).substr(4));
        else break;
    }
    auto old = resolve(target).value_or(ObjectId{});
    if (expected_old && old != *expected_old)
        throw Error("ref " + target + " changed underneath us");
    {
        LockFile lf(gitdir_ / git_to_path(target));
        lf.write(id.hex() + "\n");
        lf.commit();
    }
    append_reflog(target, old, id, who, msg);
    if (target != name) append_reflog(name, old, id, who, msg);
    else if (head_target() == target) append_reflog("HEAD", old, id, who, msg);
}

void RefDB::set_symbolic(const std::string& name, const std::string& target) {
    LockFile lf(gitdir_ / git_to_path(name));
    lf.write("ref: " + target + "\n");
    lf.commit();
}

void RefDB::remove(const std::string& name) {
    std::error_code ec;
    fs::remove(gitdir_ / git_to_path(name), ec);
    fs::remove(gitdir_ / "logs" / git_to_path(name), ec);
    auto p = packed();
    if (p.erase(name)) write_packed(p);
    // prune empty parent dirs under refs/
    auto dir = (gitdir_ / git_to_path(name)).parent_path();
    while (dir != gitdir_ / "refs" && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
        fs::remove(dir, ec);
        dir = dir.parent_path();
    }
}

void RefDB::rename(const std::string& from, const std::string& to, const Signature& who) {
    auto id = resolve(from);
    if (!id) throw Error("no such ref " + from);
    if (resolve(to)) throw Error("a ref named " + shorten_ref(to) + " already exists");
    auto log = reflog(from);
    bool was_head = head_target() == from;
    remove(from);
    {
        LockFile lf(gitdir_ / git_to_path(to));
        lf.write(id->hex() + "\n");
        lf.commit();
    }
    write_reflog(to, log);
    append_reflog(to, *id, *id, who, "Branch: renamed " + from + " to " + to);
    if (was_head) set_symbolic("HEAD", to);
}

void RefDB::append_reflog(const std::string& name, const ObjectId& o, const ObjectId& n,
                          const Signature& who, const std::string& msg) {
    auto p = gitdir_ / "logs" / git_to_path(name);
    fs::create_directories(p.parent_path());
    std::string m = msg;
    std::replace(m.begin(), m.end(), '\n', ' ');
    std::string line = o.hex() + " " + n.hex() + " " + who.format() + "\t" + m + "\n";
    auto existing = try_read_file(p).value_or("");
    write_file(p, existing + line);
}

std::vector<RefDB::ReflogEntry> RefDB::reflog(const std::string& name) const {
    std::vector<ReflogEntry> out;
    auto d = try_read_file(gitdir_ / "logs" / git_to_path(name));
    if (!d) return out;
    for (auto line : split_lines(*d)) {
        if (line.size() < 82) continue;
        ReflogEntry e;
        auto o = ObjectId::try_hex(line.substr(0, 40));
        auto n = ObjectId::try_hex(line.substr(41, 40));
        if (!o || !n) continue;
        e.old_id = *o;
        e.new_id = *n;
        auto rest = line.substr(82);
        size_t tab = rest.find('\t');
        e.who = Signature::parse(rest.substr(0, tab));
        if (tab != std::string_view::npos) e.message = std::string(rest.substr(tab + 1));
        out.push_back(std::move(e));
    }
    return out;
}

void RefDB::write_reflog(const std::string& name, const std::vector<ReflogEntry>& entries) {
    auto p = gitdir_ / "logs" / git_to_path(name);
    if (entries.empty()) {
        std::error_code ec;
        fs::remove(p, ec);
        return;
    }
    std::string s;
    for (auto& e : entries)
        s += e.old_id.hex() + " " + e.new_id.hex() + " " + e.who.format() + "\t" + e.message + "\n";
    fs::create_directories(p.parent_path());
    write_file(p, s);
}

std::optional<std::string> RefDB::dwim(const std::string& s) const {
    if (s == "HEAD") return s;
    for (auto fmt : {"%s", "refs/%s", "refs/tags/%s", "refs/heads/%s", "refs/remotes/%s", "refs/remotes/%s/HEAD"}) {
        std::string f = fmt;
        auto pos = f.find("%s");
        std::string full = f.substr(0, pos) + s + f.substr(pos + 2);
        if (starts_with(full, "refs/") || full == "HEAD")
            if (resolve(full)) return full;
    }
    return std::nullopt;
}

// ================================================================ config
fs::path home_dir() {
#ifdef _WIN32
    if (auto h = std::getenv("USERPROFILE")) return fs::path(h);
#endif
    if (auto h = std::getenv("HOME")) return fs::path(h);
    return fs::current_path();
}

fs::path global_config_path() {
    if (auto x = std::getenv("GIT_CONFIG_GLOBAL")) return fs::path(x);
    return home_dir() / ".gitconfig";
}

fs::path xdg_config_path() {
    if (auto x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "git" / "config";
    return home_dir() / ".config" / "git" / "config";
}

std::vector<fs::path> system_config_paths() {
    std::vector<fs::path> out;
#ifdef _WIN32
    for (auto env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"})
        if (auto p = std::getenv(env)) out.push_back(fs::path(p) / "Git" / "etc" / "gitconfig");
    if (auto p = std::getenv("ProgramData")) out.push_back(fs::path(p) / "Git" / "config");
#else
    out.push_back("/etc/gitconfig");
    out.push_back("/opt/homebrew/etc/gitconfig");
    out.push_back("/usr/local/etc/gitconfig");
#endif
    return out;
}

static std::string unquote_value(std::string_view v) {
    std::string out;
    bool inq = false;
    std::string pending_ws;
    for (size_t i = 0; i < v.size(); i++) {
        char c = v[i];
        if (!inq && (c == '#' || c == ';')) break;
        if (c == '"') { inq = !inq; continue; }
        if (c == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            out += pending_ws; pending_ws.clear();
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'b' ? '\b' : n;
            continue;
        }
        if (!inq && (c == ' ' || c == '\t')) { if (!out.empty()) pending_ws += c; continue; }
        out += pending_ws; pending_ws.clear();
        out += c;
    }
    return out;
}

static std::string quote_value(const std::string& v) {
    bool need = v.empty() ? false : (v.front() == ' ' || v.back() == ' ' ||
                                     v.find_first_of("#;\"") != std::string::npos);
    std::string out;
    for (char c : v) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return need ? "\"" + out + "\"" : out;
}

Config Config::load(const fs::path& file) {
    Config c;
    c.path_ = file;
    auto data = try_read_file(file);
    if (!data) return c;
    std::string sec, sub;
    for (auto lv : split_lines(*data)) {
        Line l;
        l.raw = std::string(lv);
        if (!l.raw.empty() && l.raw.back() == '\r') l.raw.pop_back();
        std::string t = trim(l.raw);
        if (!t.empty() && t[0] == '[') {
            size_t close = t.find(']');
            std::string inner = t.substr(1, close == std::string::npos ? std::string::npos : close - 1);
            size_t q = inner.find('"');
            if (q != std::string::npos) {
                sec = to_lower(trim(inner.substr(0, q)));
                size_t q2 = inner.rfind('"');
                sub = inner.substr(q + 1, q2 > q ? q2 - q - 1 : 0);
                // unescape
                std::string u;
                for (size_t i = 0; i < sub.size(); i++) {
                    if (sub[i] == '\\' && i + 1 < sub.size()) i++;
                    u += sub[i];
                }
                sub = u;
            } else {
                size_t dot = inner.find('.');
                if (dot != std::string::npos) {  // legacy [section.sub]
                    sec = to_lower(trim(inner.substr(0, dot)));
                    sub = trim(inner.substr(dot + 1));
                } else {
                    sec = to_lower(trim(inner));
                    sub.clear();
                }
            }
            l.is_header = true;
            l.section = sec;
            l.sub = sub;
            // a key may follow on the same line after ']'
            c.lines_.push_back(l);
            continue;
        }
        l.section = sec;
        l.sub = sub;
        if (!t.empty() && t[0] != '#' && t[0] != ';') {
            size_t eq = t.find('=');
            l.is_kv = true;
            l.name = to_lower(trim(t.substr(0, eq)));
            l.value = eq == std::string::npos ? "true" : unquote_value(trim(t.substr(eq + 1)));
        }
        c.lines_.push_back(std::move(l));
    }
    return c;
}

void Config::save() const {
    std::string s;
    for (auto& l : lines_) s += l.raw + "\n";
    if (path_.has_parent_path()) fs::create_directories(path_.parent_path());
    LockFile lf(path_);
    lf.write(s);
    lf.commit();
}

void Config::split_key(const std::string& key, std::string& sec, std::string& sub, std::string& name) {
    size_t first = key.find('.'), last = key.rfind('.');
    if (first == std::string::npos) throw Error("bad config key " + key);
    sec = to_lower(key.substr(0, first));
    name = to_lower(key.substr(last + 1));
    sub = first == last ? "" : key.substr(first + 1, last - first - 1);
}

std::optional<std::string> Config::get(const std::string& key) const {
    auto all = get_all(key);
    if (all.empty()) return std::nullopt;
    return all.back();
}

std::vector<std::string> Config::get_all(const std::string& key) const {
    std::string sec, sub, name;
    split_key(key, sec, sub, name);
    std::vector<std::string> out;
    for (auto& l : lines_)
        if (l.is_kv && l.section == sec && l.sub == sub && l.name == name) out.push_back(l.value);
    return out;
}

bool Config::get_bool(const std::string& key, bool def) const {
    auto v = get(key);
    if (!v) return def;
    auto s = to_lower(*v);
    if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
    if (s == "false" || s == "no" || s == "off" || s == "0" || s.empty()) return false;
    return def;
}

void Config::set(const std::string& key, const std::string& value) {
    std::string sec, sub, name;
    split_key(key, sec, sub, name);
    // replace last existing occurrence, drop earlier ones
    int last = -1;
    for (int i = 0; i < int(lines_.size()); i++)
        if (lines_[i].is_kv && lines_[i].section == sec && lines_[i].sub == sub && lines_[i].name == name) last = i;
    if (last >= 0) {
        auto& l = lines_[last];
        l.value = value;
        l.raw = "\t" + key.substr(key.rfind('.') + 1) + " = " + quote_value(value);
        return;
    }
    add(key, value);
}

void Config::add(const std::string& key, const std::string& value) {
    std::string sec, sub, name;
    split_key(key, sec, sub, name);
    Line l;
    l.is_kv = true;
    l.section = sec;
    l.sub = sub;
    l.name = name;
    l.value = value;
    l.raw = "\t" + key.substr(key.rfind('.') + 1) + " = " + quote_value(value);
    // insert after last line of the matching section
    int insert_at = -1;
    for (int i = 0; i < int(lines_.size()); i++)
        if (lines_[i].section == sec && lines_[i].sub == sub) insert_at = i;
    if (insert_at < 0) {
        Line h;
        h.is_header = true;
        h.section = sec;
        h.sub = sub;
        std::string orig_sec = key.substr(0, key.find('.'));
        h.raw = sub.empty() ? "[" + orig_sec + "]" : "[" + orig_sec + " \"" + sub + "\"]";
        lines_.push_back(h);
        lines_.push_back(l);
    } else {
        lines_.insert(lines_.begin() + insert_at + 1, l);
    }
}

void Config::unset(const std::string& key) {
    std::string sec, sub, name;
    split_key(key, sec, sub, name);
    lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                                [&](const Line& l) {
                                    return l.is_kv && l.section == sec && l.sub == sub && l.name == name;
                                }),
                 lines_.end());
}

void Config::remove_section(const std::string& section, const std::string& sub) {
    auto s = to_lower(section);
    lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                                [&](const Line& l) { return l.section == s && l.sub == sub; }),
                 lines_.end());
}

void Config::rename_section(const std::string& section, const std::string& from, const std::string& to) {
    auto s = to_lower(section);
    for (auto& l : lines_) {
        if (l.section != s || l.sub != from) continue;
        l.sub = to;
        if (l.is_header) l.raw = "[" + section + " \"" + to + "\"]";
    }
}

std::vector<std::string> Config::subsections(const std::string& section) const {
    std::vector<std::string> out;
    auto s = to_lower(section);
    for (auto& l : lines_)
        if (l.is_header && l.section == s && !l.sub.empty() &&
            std::find(out.begin(), out.end(), l.sub) == out.end())
            out.push_back(l.sub);
    return out;
}

}  // namespace git
