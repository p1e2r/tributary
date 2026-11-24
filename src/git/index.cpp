// SPDX-License-Identifier: MIT
#include "index.hpp"

#include <cctype>
#include <algorithm>

namespace git {

static uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

static bool entry_less(const IndexEntry& a, const IndexEntry& b) {
    int c = a.path.compare(b.path);
    if (c) return c < 0;
    return a.stage < b.stage;
}

Index Index::load(const fs::path& file) {
    Index idx;
    auto data = try_read_file(file);
    if (!data) return idx;
    auto d = reinterpret_cast<const uint8_t*>(data->data());
    size_t n = data->size();
    if (n < 12 + 20 || std::memcmp(d, "DIRC", 4) != 0) throw Error("index file is corrupt");
    uint32_t version = rd32(d + 4), count = rd32(d + 8);
    if (version < 2 || version > 4) throw Error("unsupported index version " + std::to_string(version));
    size_t pos = 12;
    std::string prev;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 62 > n) throw Error("index truncated");
        const uint8_t* p = d + pos;
        IndexEntry e;
        e.st.ctime_s = rd32(p); e.st.ctime_ns = rd32(p + 4);
        e.st.mtime_s = rd32(p + 8); e.st.mtime_ns = rd32(p + 12);
        e.st.dev = rd32(p + 16); e.st.ino = rd32(p + 20);
        e.mode = rd32(p + 24);
        e.st.uid = rd32(p + 28); e.st.gid = rd32(p + 32); e.st.size = rd32(p + 36);
        e.id = ObjectId::from_raw(p + 40);
        uint16_t flags = rd16(p + 60);
        e.assume_valid = flags & 0x8000;
        e.stage = (flags >> 12) & 3;
        size_t off = 62;
        if ((flags & 0x4000) && version >= 3) {
            uint16_t ext = rd16(p + 62);
            e.skip_worktree = ext & 0x4000;
            e.intent_to_add = ext & 0x2000;
            off += 2;
        }
        if (version == 4) {
            const uint8_t* q = p + off;
            uint64_t strip = *q & 0x7F;
            while (*q++ & 0x80) strip = ((strip + 1) << 7) | (*q & 0x7F);
            size_t nul = std::string_view(reinterpret_cast<const char*>(q), size_t(d + n - q)).find('\0');
            std::string suffix(reinterpret_cast<const char*>(q), nul);
            e.path = prev.substr(0, prev.size() - std::min<size_t>(strip, prev.size())) + suffix;
            pos = size_t(q - d) + nul + 1;
        } else {
            const char* name = reinterpret_cast<const char*>(p + off);
            size_t len = flags & 0xFFF;
            if (len == 0xFFF) len = std::strlen(name);
            e.path.assign(name, len);
            size_t entry_len = off + len + 1;
            entry_len = (entry_len + 7) & ~size_t(7);
            pos += entry_len;
        }
        prev = e.path;
        e.st.mode = e.mode;
        idx.entries.push_back(std::move(e));
    }
    // extensions are ignored (cache-tree is rebuilt on write)
    return idx;
}

void Index::save(const fs::path& file) {
    sort();
    bool need_v3 = false;
    for (auto& e : entries) if (e.skip_worktree || e.intent_to_add) need_v3 = true;
    Bytes out = "DIRC";
    auto put32 = [&](uint32_t v) {
        out.push_back(char(v >> 24)); out.push_back(char(v >> 16));
        out.push_back(char(v >> 8)); out.push_back(char(v));
    };
    auto put16 = [&](uint16_t v) { out.push_back(char(v >> 8)); out.push_back(char(v)); };
    put32(need_v3 ? 3 : 2);
    put32(uint32_t(entries.size()));
    for (auto& e : entries) {
        size_t start = out.size();
        put32(e.st.ctime_s); put32(e.st.ctime_ns); put32(e.st.mtime_s); put32(e.st.mtime_ns);
        put32(e.st.dev); put32(e.st.ino); put32(e.mode); put32(e.st.uid); put32(e.st.gid);
        put32(e.st.size);
        out.append(reinterpret_cast<const char*>(e.id.b.data()), 20);
        bool ext = e.skip_worktree || e.intent_to_add;
        uint16_t flags = uint16_t(std::min<size_t>(e.path.size(), 0xFFF));
        flags |= uint16_t(e.stage << 12);
        if (e.assume_valid) flags |= 0x8000;
        if (ext) flags |= 0x4000;
        put16(flags);
        if (ext) put16(uint16_t((e.skip_worktree ? 0x4000 : 0) | (e.intent_to_add ? 0x2000 : 0)));
        out += e.path;
        size_t len = out.size() - start + 1;
        size_t padded = (len + 7) & ~size_t(7);
        out.append(padded - len + 1, '\0');
    }
    Sha1 h;
    h.update(out);
    auto sum = h.finish();
    out.append(reinterpret_cast<const char*>(sum.b.data()), 20);
    LockFile lf(file);
    lf.write(out);
    lf.commit();
}

void Index::sort() { std::sort(entries.begin(), entries.end(), entry_less); }

const IndexEntry* Index::find(std::string_view path, int stage) const {
    IndexEntry key;
    key.path = std::string(path);
    key.stage = stage;
    auto it = std::lower_bound(entries.begin(), entries.end(), key, entry_less);
    if (it != entries.end() && it->path == path && it->stage == stage) return &*it;
    return nullptr;
}

IndexEntry* Index::find_mut(std::string_view path, int stage) {
    return const_cast<IndexEntry*>(static_cast<const Index*>(this)->find(path, stage));
}

void Index::add(IndexEntry e) {
    remove(e.path);
    // a file replaces a directory of the same name and vice versa
    remove_dir(e.path);
    for (size_t slash = e.path.find('/'); slash != std::string::npos; slash = e.path.find('/', slash + 1))
        remove(e.path.substr(0, slash));
    auto it = std::lower_bound(entries.begin(), entries.end(), e, entry_less);
    entries.insert(it, std::move(e));
}

void Index::remove(std::string_view path) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const IndexEntry& e) { return e.path == path; }),
                  entries.end());
}

void Index::remove_dir(std::string_view dir) {
    std::string pre = std::string(dir) + "/";
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const IndexEntry& e) { return starts_with(e.path, pre); }),
                  entries.end());
}

bool Index::has_conflicts() const {
    for (auto& e : entries) if (e.stage) return true;
    return false;
}

std::vector<std::string> Index::conflicted_paths() const {
    std::vector<std::string> out;
    for (auto& e : entries)
        if (e.stage && (out.empty() || out.back() != e.path)) out.push_back(e.path);
    return out;
}

ObjectId Index::write_tree(ObjectDB& odb) const {
    if (has_conflicts()) throw Error("cannot write a tree while there are unresolved conflicts");
    // recursive build: group entries by directory
    struct Dir {
        std::map<std::string, Dir> subdirs;
        std::vector<TreeEntry> files;
    };
    Dir root;
    for (auto& e : entries) {
        if (e.intent_to_add) continue;
        Dir* d = &root;
        size_t start = 0;
        for (;;) {
            size_t slash = e.path.find('/', start);
            if (slash == std::string::npos) break;
            d = &d->subdirs[e.path.substr(start, slash - start)];
            start = slash + 1;
        }
        d->files.push_back({e.mode, e.path.substr(start), e.id});
    }
    std::function<ObjectId(const Dir&)> build = [&](const Dir& d) {
        Tree t;
        t.entries = d.files;
        for (auto& [name, sub] : d.subdirs) t.entries.push_back({040000, name, build(sub)});
        return odb.write(ObjType::Tree, t.serialize());
    };
    return build(root);
}

void flatten_tree(const ObjectDB& odb, const ObjectId& tree, const std::string& prefix,
                  std::map<std::string, FlatEntry>& out) {
    auto t = odb.read_tree(tree);
    for (auto& e : t.entries) {
        std::string p = prefix.empty() ? e.name : prefix + "/" + e.name;
        if (e.is_tree()) flatten_tree(odb, e.id, p, out);
        else out[p] = {e.mode, e.id};
    }
}

void Index::read_tree(const ObjectDB& odb, const ObjectId& tree) {
    std::map<std::string, FlatEntry> flat;
    flatten_tree(odb, tree, "", flat);
    // keep cached stat data for entries whose content didn't change
    std::map<std::string, IndexEntry> old;
    for (auto& e : entries) if (e.stage == 0) old[e.path] = e;
    entries.clear();
    for (auto& [path, fe] : flat) {
        IndexEntry e;
        auto it = old.find(path);
        if (it != old.end() && it->second.id == fe.id && it->second.mode == fe.mode) e = it->second;
        e.path = path;
        e.mode = fe.mode;
        e.id = fe.id;
        e.stage = 0;
        entries.push_back(std::move(e));
    }
    sort();
}

// ================================================================ wildmatch
// Implements git's wildmatch: '*', '?', '[...]' classes and '**'.
static bool match_class(std::string_view& pat, char c, bool& ok) {
    // pat starts after '['
    bool negate = false;
    size_t i = 0;
    if (i < pat.size() && (pat[i] == '!' || pat[i] == '^')) { negate = true; i++; }
    bool matched = false;
    bool first = true;
    while (i < pat.size() && (first || pat[i] != ']')) {
        first = false;
        if (pat[i] == '[' && i + 1 < pat.size() && pat[i + 1] == ':') {
            size_t end = pat.find(":]", i + 2);
            if (end != std::string_view::npos) {
                auto cls = pat.substr(i + 2, end - i - 2);
                unsigned char uc = (unsigned char)c;
                if ((cls == "alpha" && isalpha(uc)) || (cls == "digit" && isdigit(uc)) ||
                    (cls == "alnum" && isalnum(uc)) || (cls == "space" && isspace(uc)) ||
                    (cls == "upper" && isupper(uc)) || (cls == "lower" && islower(uc)) ||
                    (cls == "punct" && ispunct(uc)) || (cls == "xdigit" && isxdigit(uc)))
                    matched = true;
                i = end + 2;
                continue;
            }
        }
        char lo = pat[i];
        if (lo == '\\' && i + 1 < pat.size()) lo = pat[++i];
        char hi = lo;
        if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']') {
            hi = pat[i + 2];
            if (hi == '\\' && i + 3 < pat.size()) { hi = pat[i + 3]; i++; }
            i += 2;
        }
        if (c >= lo && c <= hi) matched = true;
        i++;
    }
    if (i >= pat.size()) { ok = false; return false; }  // unterminated
    pat = pat.substr(i + 1);
    ok = true;
    return matched != negate;
}

static bool wm(std::string_view p, std::string_view t, bool pathname) {
    while (!p.empty()) {
        char c = p[0];
        if (c == '*') {
            bool dbl = p.size() >= 2 && p[1] == '*';
            if (dbl && pathname) {
                // '**' : must be at segment boundary to be special
                std::string_view rest = p.substr(2);
                if (rest.empty()) return true;
                if (rest[0] == '/') {
                    rest = rest.substr(1);
                    // match zero or more directories
                    if (wm(rest, t, pathname)) return true;
                    for (size_t i = 0; i < t.size(); i++)
                        if (t[i] == '/' && wm(rest, t.substr(i + 1), pathname)) return true;
                    return false;
                }
                for (size_t i = 0; i <= t.size(); i++)
                    if (wm(rest, t.substr(i), pathname)) return true;
                return false;
            }
            std::string_view rest = p.substr(1);
            while (!rest.empty() && rest[0] == '*') rest = rest.substr(1);
            for (size_t i = 0; i <= t.size(); i++) {
                if (wm(rest, t.substr(i), pathname)) return true;
                if (i < t.size() && pathname && t[i] == '/') return false;
            }
            return false;
        }
        if (t.empty()) return false;
        if (c == '?') {
            if (pathname && t[0] == '/') return false;
            p = p.substr(1);
            t = t.substr(1);
            continue;
        }
        if (c == '[') {
            if (pathname && t[0] == '/') return false;
            std::string_view rest = p.substr(1);
            bool ok;
            bool m = match_class(rest, t[0], ok);
            if (!ok) { if (t[0] != '[') return false; p = p.substr(1); t = t.substr(1); continue; }
            if (!m) return false;
            p = rest;
            t = t.substr(1);
            continue;
        }
        if (c == '\\' && p.size() > 1) { p = p.substr(1); c = p[0]; }
        if (c != t[0]) return false;
        p = p.substr(1);
        t = t.substr(1);
    }
    return t.empty();
}

bool wildmatch(std::string_view pattern, std::string_view text, bool pathname) {
    return wm(pattern, text, pathname);
}

// ================================================================ IgnoreRules
IgnoreRules::IgnoreRules(fs::path worktree, fs::path gitdir, std::optional<fs::path> excludes_file)
    : wt_(std::move(worktree)) {
    if (excludes_file)
        if (auto d = try_read_file(*excludes_file)) add_rules_from("", *d);
    if (auto d = try_read_file(gitdir / "info" / "exclude")) add_rules_from("", *d);
}

void IgnoreRules::add_rules_from(const std::string& base, std::string_view text) {
    for (auto line : split_lines(text)) {
        std::string l(line);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        // trailing unescaped spaces are trimmed
        while (!l.empty() && l.back() == ' ' && !(l.size() >= 2 && l[l.size() - 2] == '\\')) l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        Rule r;
        r.base = base;
        if (l[0] == '!') { r.negate = true; l = l.substr(1); }
        else if (l[0] == '\\') l = l.substr(1);
        if (!l.empty() && l.back() == '/') { r.dir_only = true; l.pop_back(); }
        if (l.empty()) continue;
        if (l.find('/') != std::string::npos) {
            r.anchored = true;
            if (l[0] == '/') l = l.substr(1);
        }
        r.pattern = l;
        rules_.push_back(std::move(r));
    }
}

void IgnoreRules::load_dir(const std::string& dir) {
    if (loaded_dirs_.count(dir)) return;
    loaded_dirs_[dir] = true;
    auto p = dir.empty() ? wt_ / ".gitignore" : wt_ / git_to_path(dir) / ".gitignore";
    if (auto d = try_read_file(p)) add_rules_from(dir, *d);
}

bool IgnoreRules::ignored(const std::string& path, bool is_dir) {
    // load .gitignore for every ancestor dir
    load_dir("");
    for (size_t s = path.find('/'); s != std::string::npos; s = path.find('/', s + 1)) load_dir(path.substr(0, s));
    // If any parent dir is ignored, the path is ignored (git can't re-include
    // files inside an excluded directory).
    for (size_t s = path.find('/'); s != std::string::npos; s = path.find('/', s + 1)) {
        std::string parent = path.substr(0, s);
        bool res = false;
        for (auto& r : rules_) {
            std::string rel;
            if (!r.base.empty()) {
                if (!starts_with(parent, r.base + "/")) continue;
                rel = parent.substr(r.base.size() + 1);
            } else rel = parent;
            std::string_view target = rel;
            if (!r.anchored) { auto sl = rel.rfind('/'); if (sl != std::string::npos) target = std::string_view(rel).substr(sl + 1); }
            if (wildmatch(r.pattern, target, true)) res = !r.negate;
        }
        if (res) return true;
    }
    bool res = false;
    for (auto& r : rules_) {
        if (r.dir_only && !is_dir) continue;
        std::string rel;
        if (!r.base.empty()) {
            if (!starts_with(path, r.base + "/")) continue;
            rel = path.substr(r.base.size() + 1);
        } else rel = path;
        std::string_view target = rel;
        if (!r.anchored) {
            auto sl = rel.rfind('/');
            if (sl != std::string::npos) target = std::string_view(rel).substr(sl + 1);
        }
        if (wildmatch(r.pattern, target, true)) res = !r.negate;
    }
    return res;
}

}  // namespace git
