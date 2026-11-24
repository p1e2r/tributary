// SPDX-License-Identifier: MIT
// References (loose + packed-refs), HEAD, reflogs, and git config files.
#pragma once

#include <map>

#include "objects.hpp"

namespace git {

struct Ref {
    std::string name;     // refs/heads/main
    ObjectId target;      // resolved id
    std::string symbolic; // for symbolic refs, the ref it points to
    std::optional<ObjectId> peeled;  // for annotated tags in packed-refs
};

class RefDB {
public:
    explicit RefDB(fs::path gitdir) : gitdir_(std::move(gitdir)) {}

    // Read the raw value of a ref ("ref: refs/heads/x" or an id).
    std::optional<std::string> read_raw(const std::string& name) const;
    // Fully resolve (following symbolic refs).
    std::optional<ObjectId> resolve(const std::string& name) const;
    // The branch HEAD points to ("refs/heads/main"), or empty if detached.
    std::string head_target() const;
    bool head_detached() const { return head_target().empty(); }

    // Every ref under `prefix` (e.g. "refs/heads/").
    std::vector<Ref> list(const std::string& prefix) const;

    void update(const std::string& name, const ObjectId& id, const std::string& reflog_msg,
                const Signature& who, std::optional<ObjectId> expected_old = std::nullopt);
    void set_symbolic(const std::string& name, const std::string& target);
    void remove(const std::string& name);
    void rename(const std::string& from, const std::string& to, const Signature& who);

    void append_reflog(const std::string& name, const ObjectId& old_id, const ObjectId& new_id,
                       const Signature& who, const std::string& msg);
    struct ReflogEntry { ObjectId old_id, new_id; Signature who; std::string message; };
    std::vector<ReflogEntry> reflog(const std::string& name) const;  // oldest first
    void write_reflog(const std::string& name, const std::vector<ReflogEntry>& entries);

    // Expand a short name: "main" -> refs/heads/main, "origin/main" -> refs/remotes/origin/main
    std::optional<std::string> dwim(const std::string& shortname) const;

private:
    std::map<std::string, Ref> packed() const;
    void write_packed(const std::map<std::string, Ref>& refs);
    fs::path gitdir_;
};

bool valid_ref_name(std::string_view name);
std::string shorten_ref(const std::string& full);

// ---------------------------------------------------------------- config
class Config {
public:
    Config() = default;
    static Config load(const fs::path& file);
    void save() const;
    // key format "section.subsection.name" or "section.name"; case-insensitive
    // section & name, case-sensitive subsection.
    std::optional<std::string> get(const std::string& key) const;
    std::vector<std::string> get_all(const std::string& key) const;
    bool get_bool(const std::string& key, bool def) const;
    void set(const std::string& key, const std::string& value);
    void add(const std::string& key, const std::string& value);
    void unset(const std::string& key);
    void remove_section(const std::string& section, const std::string& sub);
    void rename_section(const std::string& section, const std::string& from, const std::string& to);
    std::vector<std::string> subsections(const std::string& section) const;
    const fs::path& path() const { return path_; }

private:
    struct Line {
        std::string raw;             // original text (kept for round-tripping)
        std::string section, sub;    // section header context for this line
        std::string name, value;     // parsed key/value (name lowercase)
        bool is_header = false, is_kv = false;
    };
    static void split_key(const std::string& key, std::string& sec, std::string& sub, std::string& name);
    std::vector<Line> lines_;
    fs::path path_;
};

// Stack of configs, most specific first: repo, ~/.gitconfig, XDG, system.
struct ConfigStack {
    Config repo, global, xdg, system;
    std::optional<std::string> get(const std::string& k) const {
        for (auto* c : {&repo, &global, &xdg, &system})
            if (auto v = c->get(k)) return v;
        return std::nullopt;
    }
    bool get_bool(const std::string& k, bool def) const {
        for (auto* c : {&repo, &global, &xdg, &system})
            if (c->get(k)) return c->get_bool(k, def);
        return def;
    }
};
fs::path global_config_path();
fs::path xdg_config_path();
std::vector<fs::path> system_config_paths();
fs::path home_dir();

}  // namespace git
