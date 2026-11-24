// SPDX-License-Identifier: MIT
// High-level repository operations used by the app (the porcelain).
#pragma once

#include <functional>
#include <map>
#include <set>

#include "diff.hpp"
#include "index.hpp"
#include "refs.hpp"

namespace git {

enum class Change : uint8_t { None, Added, Modified, Deleted, Renamed, Untracked, Conflicted, TypeChanged };
const char* change_name(Change c);

struct RewriteStep {
    enum Op { Pick, Reword, Squash, Fixup, Drop } op = Pick;
    ObjectId commit;
    std::string message;  // for Reword / Squash (combined message)
};

struct RewriteState {
    std::string label;        // "Rebase", "Squash", "Reorder", "Cherry-pick", ...
    std::string branch;       // refs/heads/x being rewritten
    ObjectId onto, orig_head, current;
    std::vector<RewriteStep> steps;
    size_t next = 0;          // index of the step that stopped (or next to run)
    bool stopped = false;
};

struct StatusEntry {
    std::string path;
    std::string old_path;       // for renames
    Change change = Change::None;  // HEAD -> worktree, as GitHub Desktop shows it
    Change staged = Change::None;  // HEAD -> index
    Change unstaged = Change::None;  // index -> worktree
    bool conflicted = false;
    bool has_markers = false;   // conflict markers still present in the file
};

struct WorkingStatus {
    std::vector<StatusEntry> files;
    bool merging = false;
    std::optional<ObjectId> merge_head;
    std::string merge_msg;
    std::optional<RewriteState> rewrite;  // rebase/squash/... stopped on conflicts
    bool squashing = false;
    bool has_conflicts() const {
        for (auto& f : files) if (f.conflicted) return true;
        return false;
    }
};

struct CommitInfo {
    ObjectId id;
    Commit commit;
};

struct BranchInfo {
    std::string name;       // short name: "main", "origin/main"
    std::string full;       // refs/heads/main
    ObjectId tip;
    bool remote = false;
    bool current = false;
    std::string upstream;   // refs/remotes/origin/main
    int ahead = 0, behind = 0;
    int64_t tip_time = 0;
    std::string tip_summary;
};

struct Remote {
    std::string name, url, push_url;
};

struct StashInfo {
    int index;              // stash@{index}
    ObjectId id;
    std::string message;
    int64_t time;
    ObjectId base;          // HEAD at stash time
    std::string branch;
};

struct ChangedFile {
    std::string path, old_path;
    Change change;
    ObjectId old_id, new_id;
    uint32_t old_mode = 0, new_mode = 0;
};

struct CommitSelection {
    std::string path;
    std::optional<std::vector<bool>> lines;  // partial selection over full_diff_lines()
};

struct RewriteResult {
    bool conflicts = false;
    std::vector<std::string> conflicted;
    std::optional<ObjectId> new_head;
    std::string label;
};

struct MergePreview {
    enum Kind { UpToDate, FastForward, Clean, Conflicts, Unrelated } kind = UpToDate;
    int commits = 0;      // commits that would be brought in
    int conflicts = 0;
};

struct MergeOutcome {
    enum Kind { UpToDate, FastForward, Merged, Conflicts } kind;
    std::vector<std::string> conflicts;
    std::optional<ObjectId> commit;
};

class Repository {
public:
    static Repository open(const fs::path& path);         // searches upward for .git
    static std::optional<fs::path> discover(const fs::path& path);
    static Repository init(const fs::path& path, const std::string& initial_branch = "main");

    const fs::path& worktree() const { return wt_; }
    const fs::path& gitdir() const { return gitdir_; }
    ObjectDB& odb() { return *odb_; }
    RefDB& refs() { return *refs_; }
    ConfigStack& config() { return cfg_; }
    void reload_config();

    // identity
    Signature signature() const;
    std::optional<std::string> user_name() const { return cfg_.get("user.name"); }
    std::optional<std::string> user_email() const { return cfg_.get("user.email"); }

    // HEAD
    std::optional<ObjectId> head() const { return refs_->resolve("HEAD"); }
    std::string current_branch() const;  // short name or "" when detached
    bool is_unborn() const { return !head(); }

    // index
    Index load_index() const { return Index::load(gitdir_ / "index"); }
    void save_index(Index& i) const { i.save(gitdir_ / "index"); }

    // status & content
    WorkingStatus status();
    Bytes head_content(const std::string& path) const;   // "" if absent
    Bytes worktree_content(const std::string& path) const;  // normalized (LF)
    Bytes blob(const ObjectId& id) const { return odb_->read(id).data; }
    std::optional<FlatEntry> tree_entry(const ObjectId& tree, const std::string& path) const;
    FileDiff working_diff(const StatusEntry& e, int context = 3, bool ignore_ws = false);

    // commit
    ObjectId commit(const std::vector<CommitSelection>& files, const std::string& summary,
                    const std::string& description, bool amend = false,
                    const std::vector<std::pair<std::string, std::string>>& coauthors = {}, bool run_hooks = true);
    void undo_last_commit();  // soft reset to first parent
    void discard(const std::vector<std::string>& paths);
    void discard_lines(const std::string& path, const std::vector<bool>& selection);
    void add_to_gitignore(const std::string& pattern);

    // history
    std::vector<CommitInfo> log(const ObjectId& start, size_t max, size_t skip = 0,
                                const std::string& path_filter = "");
    std::vector<ChangedFile> commit_changes(const ObjectId& commit);
    FileDiff commit_file_diff(const ChangedFile& f, int context = 3, bool ignore_ws = false);
    std::vector<ChangedFile> tree_changes(const std::optional<ObjectId>& old_tree, const ObjectId& new_tree);
    bool is_ancestor(const ObjectId& ancestor, const ObjectId& of);
    std::optional<ObjectId> merge_base(const ObjectId& a, const ObjectId& b);
    std::pair<int, int> ahead_behind(const ObjectId& local, const ObjectId& upstream);

    // revisions
    std::optional<ObjectId> rev_parse(const std::string& spec);

    // branches
    std::vector<BranchInfo> branches(bool include_remote = true);
    std::string upstream_of(const std::string& branch) const;  // refs/remotes/x/y or ""
    void set_upstream(const std::string& branch, const std::string& remote, const std::string& merge_ref);
    void create_branch(const std::string& name, const std::optional<ObjectId>& start, bool checkout);
    void checkout_branch(const std::string& name);  // also handles "origin/x" -> creates tracking branch
    void checkout_commit(const ObjectId& id);       // detached HEAD
    void delete_branch(const std::string& name, bool also_remote_tracking = false);
    void rename_branch(const std::string& from, const std::string& to);
    // update worktree+index from HEAD tree to target tree, refusing to clobber local changes
    void switch_tree(const std::optional<ObjectId>& from_tree, const ObjectId& to_tree, bool force = false);

    // merge, revert, conflicts
    MergeOutcome merge(const std::string& branch_ref, bool squash = false);
    MergePreview merge_preview(const std::string& branch_ref);
    bool squash_in_progress() const;
    void abort_merge();
    void resolve_with(const std::string& path, int stage);  // 2 = ours, 3 = theirs
    ObjectId revert(const ObjectId& commit);
    ObjectId cherry_pick(const ObjectId& commit);
    void reset_hard(const ObjectId& to);
    void reset_mixed(const ObjectId& to);  // move branch, keep changes in the worktree

    // History rewriting (rebase / squash / reorder / reword / cherry-pick).
    // Rewrites `branch` (refs/heads/x) as `steps` replayed on top of `onto`.
    RewriteResult rewrite(const std::string& label, const std::string& branch, const ObjectId& onto,
                          std::vector<RewriteStep> steps);
    RewriteResult rewrite_continue();
    void rewrite_abort();
    std::optional<RewriteState> rewrite_state() const;
    // Convenience builders
    RewriteResult rebase_onto(const std::string& upstream_ref);
    RewriteResult reword(const ObjectId& commit, const std::string& message);
    RewriteResult squash_commits(const std::vector<ObjectId>& commits, const ObjectId& into, const std::string& message);
    RewriteResult reorder(const std::vector<ObjectId>& commits, const ObjectId& before);  // move commits to just before `before` (or to top if zero)
    RewriteResult cherry_pick_onto(const std::vector<ObjectId>& commits, const std::string& target_branch);
    // Commits on the current branch not on `base`, newest first.
    std::vector<ObjectId> commits_between(const ObjectId& base, const ObjectId& tip);
    void delete_tag(const std::string& name);
    fs::path hooks_dir() const;

    // stash
    std::vector<StashInfo> stashes();
    ObjectId stash_push(const std::string& message, bool include_untracked = true);
    void stash_apply(int index, bool pop);
    void stash_drop(int index);
    std::vector<ChangedFile> stash_changes(int index);

    // remotes
    std::vector<Remote> remotes() const;
    void add_remote(const std::string& name, const std::string& url);
    void set_remote_url(const std::string& name, const std::string& url);
    void remove_remote(const std::string& name);

    // tags
    std::vector<std::pair<std::string, ObjectId>> tags();
    void create_tag(const std::string& name, const ObjectId& target, const std::string& message = "");

    // helpers for transport
    std::set<ObjectId> reachable_objects(const std::vector<ObjectId>& tips, const std::set<ObjectId>& stop_commits);
    std::vector<ObjectId> local_commit_tips();

    // worktree io
    void write_worktree_file(const std::string& path, const Bytes& content, uint32_t mode);
    void remove_worktree_file(const std::string& path);
    Bytes to_worktree_eol(const std::string& path, const Bytes& data) const;
    Bytes to_odb_eol(const std::string& path, const Bytes& data) const;

    // ---------------- Git LFS (filter=lfs in .gitattributes)
    // Is this path stored in LFS (per .gitattributes / .git/info/attributes)?
    bool is_lfs(const std::string& path) const;
    Bytes worktree_blob(const std::string& path) const;  // worktree file as it would be stored (clean filters applied)
    // worktree bytes -> what goes in the object database. With store, LFS content is
    // copied into .git/lfs/objects and the pointer is returned.
    Bytes to_odb(const std::string& path, const Bytes& raw, bool store) const;
    // blob bytes -> displayable content: LFS pointers become the real file when the
    // object is available locally (diffs, image diffs).
    Bytes smudged(const std::string& path, const Bytes& blob) const;
    fs::path lfs_object_path(const std::string& oid) const;
    // Files written as pointers because their LFS object wasn't downloaded yet.
    const std::set<std::string>& lfs_missing() const { return lfs_missing_; }
    void clear_lfs_missing() { lfs_missing_.clear(); }
    void invalidate_attributes() { attr_cache_.clear(); }

    // ---------------- commit signing (commit.gpgsign, gpg.format openpgp/ssh/x509)
    bool signing_enabled() const;
    // Writes a commit object, signing it when configured. Any old signature is dropped.
    ObjectId write_commit(Commit c, bool allow_sign = true);

private:
    Repository() = default;
    void walk_untracked(const std::string& dir, IgnoreRules& ign, const Index& idx,
                        std::vector<std::string>& out);
    ObjectId hash_worktree_file(const std::string& path, const FileStat& st, uint32_t& mode_out);
    IndexEntry make_entry(const std::string& path, const ObjectId& id, uint32_t mode);
    ObjectId make_commit(const ObjectId& tree, std::vector<ObjectId> parents, const std::string& message,
                         const std::string& reflog_prefix);
    MergeOutcome three_way(const ObjectId& base_tree, const ObjectId& ours_tree, const ObjectId& theirs_tree,
                           const std::string& ours_label, const std::string& theirs_label,
                           Index& index, std::vector<std::string>& conflicts, bool dry = false);
    RewriteResult rewrite_run(RewriteState st);
    void save_rewrite_state(const RewriteState& st);
    void clear_rewrite_state();
    ObjectId commit_from_worktree(const std::vector<ObjectId>& parents, const Signature& author, const std::string& msg);
    void run_commit_hooks_pre(std::string& message);
    void run_post_commit_hook();
    void write_merge_state(const ObjectId& other, const std::string& msg);
    void clear_merge_state();

    struct AttrRule { std::string pattern; bool lfs; };
    mutable std::map<std::string, std::vector<AttrRule>> attr_cache_;
    mutable std::set<std::string> lfs_missing_;
    const std::vector<AttrRule>& attr_rules(const std::string& dir) const;
    std::string sign_payload(const std::string& payload) const;

    fs::path wt_, gitdir_;
    std::unique_ptr<ObjectDB> odb_;
    std::unique_ptr<RefDB> refs_;
    ConfigStack cfg_;
};

// Utilities
std::string format_relative_time(int64_t t, int64_t now);
bool is_valid_branch_name(const std::string& n);
std::string sanitize_branch_name(const std::string& n);

}  // namespace git
