// SPDX-License-Identifier: MIT
// The staging area (.git/index, versions 2-4) and .gitignore matching.
#pragma once

#include "objects.hpp"

namespace git {

struct IndexEntry {
    FileStat st;      // cached stat info
    ObjectId id;
    uint32_t mode = 0100644;
    int stage = 0;    // 0 normal, 1 base, 2 ours, 3 theirs (conflicts)
    bool assume_valid = false, skip_worktree = false, intent_to_add = false;
    std::string path; // '/'-separated
};

class Index {
public:
    static Index load(const fs::path& file);
    void save(const fs::path& file);  // writes version 2 (or 3 if needed)

    std::vector<IndexEntry> entries;  // sorted by (path, stage)

    const IndexEntry* find(std::string_view path, int stage = 0) const;
    IndexEntry* find_mut(std::string_view path, int stage = 0);
    void add(IndexEntry e);            // replaces existing stage-0 + clears conflict stages
    void remove(std::string_view path); // all stages
    void remove_dir(std::string_view dirpath);
    bool has_conflicts() const;
    std::vector<std::string> conflicted_paths() const;
    void sort();

    // Build tree objects from the stage-0 entries; returns the root tree id.
    ObjectId write_tree(ObjectDB& odb) const;
    // Replace contents with a tree (like `git read-tree`).
    void read_tree(const ObjectDB& odb, const ObjectId& tree);
};

// Flattens a tree into path -> (mode, id).
struct FlatEntry { uint32_t mode; ObjectId id; };
void flatten_tree(const ObjectDB& odb, const ObjectId& tree, const std::string& prefix,
                  std::map<std::string, FlatEntry>& out);

// ---------------------------------------------------------------- ignore
bool wildmatch(std::string_view pattern, std::string_view text, bool pathname);

class IgnoreRules {
public:
    // Loads .git/info/exclude and core.excludesFile; per-dir .gitignore files
    // are loaded lazily as directories are visited.
    IgnoreRules(fs::path worktree, fs::path gitdir, std::optional<fs::path> excludes_file);
    bool ignored(const std::string& path, bool is_dir);
    void add_rules_from(const std::string& base_dir, std::string_view text);

private:
    struct Rule {
        std::string base;     // directory the .gitignore lives in ("" = root)
        std::string pattern;
        bool negate = false, dir_only = false, anchored = false;
    };
    void load_dir(const std::string& dir);
    fs::path wt_;
    std::vector<Rule> rules_;
    std::map<std::string, bool> loaded_dirs_;
};

}  // namespace git
