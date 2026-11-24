// SPDX-License-Identifier: MIT
#include "repo.hpp"
#include "lfs.hpp"
#include "process.hpp"

#include <cctype>
#include <cstdlib>
#include <tuple>
#include <algorithm>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace git {

const char* change_name(Change c) {
    switch (c) {
        case Change::Added: return "Added";
        case Change::Modified: return "Modified";
        case Change::Deleted: return "Deleted";
        case Change::Renamed: return "Renamed";
        case Change::Untracked: return "Untracked";
        case Change::Conflicted: return "Conflicted";
        case Change::TypeChanged: return "Type changed";
        default: return "";
    }
}

// ================================================================ open/init
std::optional<fs::path> Repository::discover(const fs::path& start) {
    std::error_code ec;
    fs::path p = fs::absolute(start, ec);
    while (true) {
        auto dotgit = p / ".git";
        if (fs::is_directory(dotgit, ec) && fs::exists(dotgit / "HEAD", ec)) return p;
        if (fs::is_regular_file(dotgit, ec)) return p;  // gitfile (worktrees, submodules)
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return std::nullopt;
}

Repository Repository::open(const fs::path& path) {
    auto wt = discover(path);
    if (!wt) throw Error("not a git repository: " + path_to_git(path));
    Repository r;
    r.wt_ = fs::canonical(*wt);
    r.gitdir_ = r.wt_ / ".git";
    if (fs::is_regular_file(r.gitdir_)) {
        auto t = trim(read_file(r.gitdir_));
        if (!starts_with(t, "gitdir:")) throw Error("invalid .git file");
        fs::path gd = git_to_path(trim(t.substr(7)));
        if (gd.is_relative()) gd = r.wt_ / gd;
        r.gitdir_ = fs::canonical(gd);
    }
    fs::path objects = r.gitdir_ / "objects";
    fs::path common = r.gitdir_;
    if (auto cd = try_read_file(r.gitdir_ / "commondir")) {  // linked worktree
        fs::path c = git_to_path(trim(*cd));
        if (c.is_relative()) c = r.gitdir_ / c;
        common = fs::canonical(c);
        objects = common / "objects";
    }
    r.odb_ = std::make_unique<ObjectDB>(objects);
    r.refs_ = std::make_unique<RefDB>(r.gitdir_);
    r.reload_config();
    return r;
}

Repository Repository::init(const fs::path& path, const std::string& initial_branch) {
    fs::create_directories(path);
    auto g = path / ".git";
    if (fs::exists(g / "HEAD")) return open(path);
    fs::create_directories(g / "objects" / "info");
    fs::create_directories(g / "objects" / "pack");
    fs::create_directories(g / "refs" / "heads");
    fs::create_directories(g / "refs" / "tags");
    fs::create_directories(g / "info");
    write_file(g / "HEAD", "ref: refs/heads/" + initial_branch + "\n");
    write_file(g / "description", "Unnamed repository; edit this file 'description' to name the repository.\n");
    write_file(g / "info" / "exclude", "# git ls-files --others --exclude-from=.git/info/exclude\n");
    std::string cfg = "[core]\n\trepositoryformatversion = 0\n\tfilemode = ";
#ifdef _WIN32
    cfg += "false\n\tbare = false\n\tlogallrefupdates = true\n\tsymlinks = false\n\tignorecase = true\n";
#elif defined(__APPLE__)
    cfg += "true\n\tbare = false\n\tlogallrefupdates = true\n\tignorecase = true\n\tprecomposeunicode = true\n";
#else
    cfg += "true\n\tbare = false\n\tlogallrefupdates = true\n";
#endif
    write_file(g / "config", cfg);
    return open(path);
}

void Repository::reload_config() {
    cfg_.repo = Config::load(gitdir_ / "config");
    cfg_.global = Config::load(global_config_path());
    cfg_.xdg = Config::load(xdg_config_path());
    cfg_.system = Config{};
    if (!std::getenv("GIT_CONFIG_NOSYSTEM"))
        for (auto& p : system_config_paths()) {
            std::error_code ec;
            if (fs::exists(p, ec)) { cfg_.system = Config::load(p); break; }
        }
}

Signature Repository::signature() const {
    auto name = cfg_.get("user.name"), email = cfg_.get("user.email");
    if (!name || !email) throw Error("Please set your name and email in Settings before committing.");
    return Signature::now(*name, *email);
}

std::string Repository::current_branch() const {
    auto t = refs_->head_target();
    if (starts_with(t, "refs/heads/")) return t.substr(11);
    return "";
}

// ================================================================ eol
static bool autocrlf_text(std::string_view d) { return !std::memchr(d.data(), '\0', std::min<size_t>(d.size(), 8000)); }

Bytes Repository::to_odb_eol(const std::string&, const Bytes& data) const {
    auto ac = to_lower(cfg_.get("core.autocrlf").value_or("false"));
    if ((ac != "true" && ac != "input") || !autocrlf_text(data)) return data;
    if (data.find('\r') == Bytes::npos) return data;
    Bytes out;
    out.reserve(data.size());
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') continue;
        out += data[i];
    }
    return out;
}

Bytes Repository::to_worktree_eol(const std::string&, const Bytes& data) const {
    auto ac = to_lower(cfg_.get("core.autocrlf").value_or("false"));
    if (ac != "true" || !autocrlf_text(data)) return data;
    Bytes out;
    out.reserve(data.size() + data.size() / 32);
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) out += '\r';
        out += data[i];
    }
    return out;
}

// ================================================================ worktree io
static bool base_name_is_gitattributes(const std::string& p) {
    return p == ".gitattributes" || (p.size() > 15 && p.compare(p.size() - 15, 15, "/.gitattributes") == 0);
}

void Repository::write_worktree_file(const std::string& path, const Bytes& content, uint32_t mode) {
    auto p = wt_ / git_to_path(path);
    std::error_code ec;
    // a directory may be in the way (or a file where we need a directory)
    if (fs::is_directory(p, ec) && !fs::is_symlink(p, ec)) fs::remove_all(p, ec);
    for (auto parent = p.parent_path(); parent != wt_ && parent.has_parent_path(); parent = parent.parent_path()) {
        if (fs::exists(parent, ec) && !fs::is_directory(parent, ec)) { fs::remove(parent, ec); break; }
    }
    fs::create_directories(p.parent_path(), ec);
    if (mode == 0120000) {
        fs::remove(p, ec);
#ifndef _WIN32
        fs::create_symlink(git_to_path(content), p, ec);
        if (!ec) return;
#endif
        write_file(p, content);
        return;
    }
    if (fs::is_symlink(p, ec)) fs::remove(p, ec);
    if (content.size() < 1024) {
        if (auto ptr = lfs::parse_pointer(content)) {
            // Git LFS smudge: the real file if we have it, else the pointer until `lfs pull`
            if (auto real = try_read_file(lfs_object_path(ptr->oid))) write_file(p, *real);
            else { write_file(p, content); lfs_missing_.insert(path); }
#ifndef _WIN32
            fs::permissions(p, mode == 0100755 ? fs::perms(0755) : fs::perms(0644), ec);
#endif
            return;
        }
    }
    if (base_name_is_gitattributes(path)) attr_cache_.clear();
    write_file(p, to_worktree_eol(path, content));
#ifndef _WIN32
    auto perms = mode == 0100755 ? fs::perms(0755) : fs::perms(0644);
    fs::permissions(p, perms, ec);
#endif
}

void Repository::remove_worktree_file(const std::string& path) {
    auto p = wt_ / git_to_path(path);
    std::error_code ec;
    fs::remove(p, ec);
    for (auto d = p.parent_path(); d != wt_ && starts_with(path_to_git(d), path_to_git(wt_)); d = d.parent_path()) {
        if (fs::is_directory(d, ec) && fs::is_empty(d, ec)) fs::remove(d, ec);
        else break;
    }
}

ObjectId Repository::hash_worktree_file(const std::string& path, const FileStat& st, uint32_t& mode) {
    auto p = wt_ / git_to_path(path);
    bool filemode = cfg_.get_bool("core.filemode", true);
    if (st.is_link) {
        mode = 0120000;
        std::error_code ec;
        auto target = fs::read_symlink(p, ec);
        return hash_object(ObjType::Blob, path_to_git(target));
    }
    mode = (filemode && st.is_exec) ? 0100755 : 0100644;
    return hash_object(ObjType::Blob, to_odb(path, read_file(p), false));
}

IndexEntry Repository::make_entry(const std::string& path, const ObjectId& id, uint32_t mode) {
    IndexEntry e;
    e.path = path;
    e.id = id;
    e.mode = mode;
    if (auto st = stat_path(wt_ / git_to_path(path))) e.st = *st;
    e.st.mode = mode;
    return e;
}

// ================================================================ status
void Repository::walk_untracked(const std::string& dir, IgnoreRules& ign, const Index& idx,
                                std::vector<std::string>& out) {
    std::error_code ec;
    fs::path d = dir.empty() ? wt_ : wt_ / git_to_path(dir);
    std::vector<fs::directory_entry> ents;
    for (auto& e : fs::directory_iterator(d, ec)) ents.push_back(e);
    for (auto& e : ents) {
        auto name = path_to_git(e.path().filename());
        if (name == ".git") continue;
        std::string rel = dir.empty() ? name : dir + "/" + name;
        bool is_dir = e.is_directory(ec) && !e.is_symlink(ec);
        if (is_dir) {
            // nested repository (or submodule): don't descend
            if (fs::exists(e.path() / ".git", ec)) {
                if (!idx.find(rel) && !ign.ignored(rel, true)) out.push_back(rel + "/");
                continue;
            }
            if (ign.ignored(rel, true)) {
                // still descend if tracked files live in it
                std::string pre = rel + "/";
                bool tracked_inside = false;
                for (auto& ie : idx.entries) if (starts_with(ie.path, pre)) { tracked_inside = true; break; }
                if (!tracked_inside) continue;
            }
            walk_untracked(rel, ign, idx, out);
        } else {
            if (idx.find(rel) || idx.find(rel, 1) || idx.find(rel, 2) || idx.find(rel, 3)) continue;
            if (ign.ignored(rel, false)) continue;
            out.push_back(rel);
        }
    }
}

WorkingStatus Repository::status() {
    WorkingStatus ws;
    Index idx = load_index();
    std::map<std::string, FlatEntry> head_files;
    if (auto h = head()) flatten_tree(*odb_, odb_->read_commit(*h).tree, "", head_files);

    auto idx_mtime = stat_path(gitdir_ / "index");
    bool dirty_cache = false;
    // worktree id per tracked path (after comparing with index)
    std::map<std::string, std::pair<std::optional<ObjectId>, uint32_t>> wt_ids;
    std::map<std::string, StatusEntry> entries;

    for (auto& e : idx.entries) {
        if (e.stage != 0) {
            auto& se = entries[e.path];
            se.path = e.path;
            se.conflicted = true;
            se.change = Change::Conflicted;
            continue;
        }
        if (e.skip_worktree) { wt_ids[e.path] = {e.id, e.mode}; continue; }
        auto st = stat_path(wt_ / git_to_path(e.path));
        if (!st || st->is_dir) { wt_ids[e.path] = {std::nullopt, 0}; continue; }
        bool racy = idx_mtime && e.st.mtime_s >= idx_mtime->mtime_s;
        bool same_stat = e.st.mtime_s == st->mtime_s && e.st.mtime_ns == st->mtime_ns &&
                         e.st.size == st->size && (st->ino == 0 || e.st.ino == st->ino);
        if (same_stat && !racy) {
            uint32_t m = e.mode;
            if (cfg_.get_bool("core.filemode", true) && !st->is_link && e.mode != 0120000)
                m = st->is_exec ? 0100755 : 0100644;
            wt_ids[e.path] = {e.id, m};
            continue;
        }
        uint32_t mode;
        auto id = hash_worktree_file(e.path, *st, mode);
        if (!cfg_.get_bool("core.filemode", true) && e.mode != 0120000) mode = e.mode;
        wt_ids[e.path] = {id, mode};
        if (id == e.id && !racy) {
            // refresh cached stat so the next status is fast
            auto* me = idx.find_mut(e.path);
            me->st = *st;
            me->st.mode = me->mode;
            dirty_cache = true;
        }
    }

    // index vs head (staged) and worktree vs index (unstaged), plus combined
    std::set<std::string> all_paths;
    for (auto& [p, _] : head_files) all_paths.insert(p);
    for (auto& e : idx.entries) all_paths.insert(e.path);
    for (auto& p : all_paths) {
        if (entries.count(p) && entries[p].conflicted) continue;
        auto hit = head_files.find(p);
        const IndexEntry* ie = idx.find(p);
        StatusEntry se;
        se.path = p;
        if (hit == head_files.end() && ie) se.staged = Change::Added;
        else if (hit != head_files.end() && !ie) se.staged = Change::Deleted;
        else if (hit != head_files.end() && ie && (hit->second.id != ie->id || hit->second.mode != ie->mode))
            se.staged = Change::Modified;
        std::optional<ObjectId> wid;
        uint32_t wmode = 0;
        if (ie) {
            auto it = wt_ids.find(p);
            if (it != wt_ids.end()) { wid = it->second.first; wmode = it->second.second; }
            if (!wid) se.unstaged = Change::Deleted;
            else if (*wid != ie->id || wmode != ie->mode) se.unstaged = Change::Modified;
        }
        // combined HEAD -> worktree
        if (hit == head_files.end()) {
            if (ie && wid) se.change = Change::Added;
        } else {
            if (!ie || !wid) se.change = Change::Deleted;
            else if (*wid != hit->second.id || wmode != hit->second.mode)
                se.change = ((hit->second.mode == 0120000) != (wmode == 0120000)) ? Change::TypeChanged : Change::Modified;
        }
        if (se.change != Change::None || se.staged != Change::None || se.unstaged != Change::None)
            entries[p] = se;
    }
    // untracked
    std::optional<fs::path> excl;
    if (auto x = cfg_.get("core.excludesfile")) {
        std::string s = *x;
        if (starts_with(s, "~/")) excl = home_dir() / git_to_path(s.substr(2));
        else excl = git_to_path(s);
    } else {
        auto xdg = std::getenv("XDG_CONFIG_HOME");
        excl = (xdg ? fs::path(xdg) : home_dir() / ".config") / "git" / "ignore";
    }
    IgnoreRules ign(wt_, gitdir_, excl);
    std::vector<std::string> untracked;
    walk_untracked("", ign, idx, untracked);
    for (auto& u : untracked) {
        StatusEntry se;
        se.path = u;
        se.change = Change::Untracked;
        entries[u] = se;
    }
    // exact rename detection: deleted + untracked/added with same content
    std::unordered_map<ObjectId, std::string, ObjectIdHash> deleted_by_id;
    for (auto& [p, se] : entries)
        if (se.change == Change::Deleted) deleted_by_id[head_files[p].id] = p;
    if (!deleted_by_id.empty()) {
        std::vector<std::string> remove;
        for (auto& [p, se] : entries) {
            if (se.change != Change::Untracked && se.change != Change::Added) continue;
            if (!p.empty() && p.back() == '/') continue;
            ObjectId id;
            if (se.change == Change::Added && wt_ids.count(p) && wt_ids[p].first) id = *wt_ids[p].first;
            else {
                auto st = stat_path(wt_ / git_to_path(p));
                if (!st || st->size > 8 * 1024 * 1024) continue;
                uint32_t m;
                id = hash_worktree_file(p, *st, m);
            }
            auto it = deleted_by_id.find(id);
            if (it == deleted_by_id.end()) continue;
            se.change = Change::Renamed;
            se.old_path = it->second;
            remove.push_back(it->second);
            deleted_by_id.erase(it);
        }
        for (auto& r : remove) entries.erase(r);
    }
    // conflict markers
    for (auto& [p, se] : entries) {
        if (!se.conflicted) continue;
        if (auto d = try_read_file(wt_ / git_to_path(p)))
            se.has_markers = d->find("<<<<<<< ") != Bytes::npos && d->find(">>>>>>> ") != Bytes::npos;
    }
    for (auto& [p, se] : entries) ws.files.push_back(se);
    ws.rewrite = rewrite_state();
    ws.squashing = fs::exists(gitdir_ / "SQUASH_MSG");
    if (ws.squashing && !ws.rewrite) ws.merge_msg = try_read_file(gitdir_ / "SQUASH_MSG").value_or("");
    if (auto mh = try_read_file(gitdir_ / "MERGE_HEAD")) {
        ws.merging = true;
        ws.merge_head = ObjectId::try_hex(trim(*mh).substr(0, 40));
        ws.merge_msg = try_read_file(gitdir_ / "MERGE_MSG").value_or("");
    }
    if (dirty_cache) {
        try { save_index(idx); } catch (...) {}
    }
    return ws;
}

Bytes Repository::head_content(const std::string& path) const {
    auto h = head();
    if (!h) return "";
    auto e = tree_entry(odb_->read_commit(*h).tree, path);
    if (!e) return "";
    return odb_->read(e->id).data;
}

Bytes Repository::worktree_content(const std::string& path) const {
    auto p = wt_ / git_to_path(path);
    std::error_code ec;
    if (fs::is_symlink(p, ec)) return path_to_git(fs::read_symlink(p, ec));
    auto d = try_read_file(p);
    if (!d) return "";
    return to_odb_eol(path, *d);
}

Bytes Repository::worktree_blob(const std::string& path) const {
    auto p = wt_ / git_to_path(path);
    std::error_code ec;
    if (fs::is_symlink(p, ec)) return path_to_git(fs::read_symlink(p, ec));
    auto d = try_read_file(p);
    if (!d) return "";
    return to_odb(path, *d, true);
}

std::optional<FlatEntry> Repository::tree_entry(const ObjectId& tree, const std::string& path) const {
    ObjectId cur = tree;
    size_t start = 0;
    for (;;) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        auto t = odb_->read_tree(cur);
        auto* e = t.find(part);
        if (!e) return std::nullopt;
        if (slash == std::string::npos) return FlatEntry{e->mode, e->id};
        if (!e->is_tree()) return std::nullopt;
        cur = e->id;
        start = slash + 1;
    }
}

FileDiff Repository::working_diff(const StatusEntry& e, int context, bool ignore_ws) {
    if (!e.path.empty() && e.path.back() == '/') return {};
    Bytes old = e.change == Change::Untracked || e.change == Change::Added ? ""
                : smudged(e.path, head_content(e.old_path.empty() ? e.path : e.old_path));
    Bytes neu = e.change == Change::Deleted ? "" : worktree_content(e.path);
    return diff_text(old, neu, context, ignore_ws);
}

// ================================================================ signing
bool Repository::signing_enabled() const { return cfg_.get_bool("commit.gpgsign", false); }

std::string Repository::sign_payload(const std::string& payload) const {
    std::string format = to_lower(cfg_.get("gpg.format").value_or("openpgp"));
    auto dir = gitdir_ / "tributary-sign";
    std::error_code ec;
    fs::create_directories(dir, ec);
    struct Cleanup { fs::path d; ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{dir};
    std::string key = cfg_.get("user.signingkey").value_or("");
    if (format == "ssh") {
        // ssh-keygen -Y sign -n git -f <key> <file>  →  <file>.sig
        std::string prog = cfg_.get("gpg.ssh.program").value_or("ssh-keygen");
        if (find_program(prog).empty()) throw Error("Commit signing with SSH needs `" + prog + "`, which wasn't found.");
        if (key.empty()) throw Error("Set user.signingkey to your SSH key to sign commits with SSH.");
        std::vector<std::string> argv = {prog, "-Y", "sign", "-n", "git", "-f"};
        if (starts_with(key, "key::") || starts_with(key, "ssh-") || starts_with(key, "ecdsa-") || starts_with(key, "sk-")) {
            // a literal public key: the private half lives in ssh-agent
            auto pub = dir / "key.pub";
            write_file(pub, (starts_with(key, "key::") ? key.substr(5) : key) + "\n");
            argv.push_back(path_to_git(pub));
            argv.push_back("-U");
        } else {
            argv.push_back(starts_with(key, "~/") ? path_to_git(home_dir()) + key.substr(1) : key);
        }
        auto file = dir / "payload";
        write_file(file, payload);
        argv.push_back(path_to_git(file));
        auto r = run_process(argv, wt_);
        auto sig = try_read_file(fs::path(path_to_git(file) + ".sig"));
        if (r.exit_code != 0 || !sig) throw Error("Couldn't sign the commit with SSH:\n\n" + trim(r.output));
        return *sig;
    }
    // OpenPGP (gpg) or X.509 (gpgsm): detached, armored signature
    std::string prog = format == "x509" ? cfg_.get("gpg.x509.program").value_or("gpgsm")
                                        : cfg_.get("gpg.openpgp.program").value_or(cfg_.get("gpg.program").value_or("gpg"));
    if (find_program(prog).empty()) throw Error("Commit signing needs `" + prog + "`, which wasn't found. Install GnuPG or turn off commit signing.");
    if (key.empty()) key = cfg_.get("user.name").value_or("") + " <" + cfg_.get("user.email").value_or("") + ">";
    auto out = dir / "sig.asc";
    auto r = run_process({prog, "--status-fd=1", "--batch", "--yes", "-bsau", key, "-o", path_to_git(out)}, wt_, {}, payload);
    auto sig = try_read_file(out);
    if (r.exit_code != 0 || !sig || r.output.find("SIG_CREATED") == std::string::npos)
        throw Error("Couldn't sign the commit with " + prog + ":\n\n" + trim(r.output));
    return *sig;
}

ObjectId Repository::write_commit(Commit c, bool allow_sign) {
    // a rewritten commit can't keep its old signature
    c.extra_headers.erase(std::remove_if(c.extra_headers.begin(), c.extra_headers.end(),
                                         [](auto& h) { return h.first == "gpgsig" || h.first == "gpgsig-sha256"; }),
                          c.extra_headers.end());
    if (allow_sign && signing_enabled()) {
        std::string sig = sign_payload(c.serialize());
        while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
        std::string clean;
        for (char ch : sig) if (ch != '\r') clean += ch;
        c.extra_headers.push_back({"gpgsig", clean});
    }
    return odb_->write(ObjType::Commit, c.serialize());
}

// ================================================================ commit
ObjectId Repository::make_commit(const ObjectId& tree, std::vector<ObjectId> parents, const std::string& message,
                                 const std::string& prefix) {
    Commit c;
    c.tree = tree;
    c.parents = std::move(parents);
    c.author = c.committer = signature();
    c.message = message;
    auto id = write_commit(c);
    std::string summary = c.summary();
    refs_->update("HEAD", id, prefix + summary, c.committer);
    return id;
}

ObjectId Repository::commit(const std::vector<CommitSelection>& files, const std::string& summary,
                            const std::string& description, bool amend,
                            const std::vector<std::pair<std::string, std::string>>& coauthors, bool run_hooks) {
    if (trim(summary).empty()) throw Error("A commit summary is required.");
    auto h = head();
    Index idx = load_index();
    auto status_now = status();
    bool merging = status_now.merging;
    // Start from HEAD's tree so only selected changes get committed (index
    // entries for unselected paths are reset to HEAD).
    Index next;
    std::optional<ObjectId> base_commit = h;
    if (amend) {
        if (!h) throw Error("nothing to amend");
        auto c = odb_->read_commit(*h);
        base_commit = c.parents.empty() ? std::nullopt : std::optional<ObjectId>(c.parents[0]);
        next.read_tree(*odb_, c.tree);
    } else if (h) {
        next.read_tree(*odb_, odb_->read_commit(*h).tree);
    }
    // carry cached stat info
    for (auto& e : next.entries)
        if (auto* old = idx.find(e.path); old && old->id == e.id) e.st = old->st;

    std::map<std::string, const StatusEntry*> by_path;
    for (auto& f : status_now.files) by_path[f.path] = &f;
    for (auto& sel : files) {
        auto it = by_path.find(sel.path);
        StatusEntry tmp;
        const StatusEntry* se = nullptr;
        if (it != by_path.end()) se = it->second;
        else { tmp.path = sel.path; tmp.change = Change::Modified; se = &tmp; }
        if (se->conflicted && se->has_markers) throw Error(sel.path + " still contains conflict markers");
        if (!se->old_path.empty()) next.remove(se->old_path);
        auto p = wt_ / git_to_path(sel.path);
        auto st = stat_path(p);
        if (!st) { next.remove(sel.path); continue; }
        if (st->is_dir) {
            // untracked nested repo shown as "dir/": add every file inside it
            continue;
        }
        uint32_t mode;
        ObjectId id;
        if (sel.lines && !is_lfs(sel.path)) {
            Bytes old = head_content(se->old_path.empty() ? sel.path : se->old_path);
            Bytes partial = apply_selection(old, worktree_content(sel.path), *sel.lines);
            id = odb_->write(ObjType::Blob, partial);
            hash_worktree_file(sel.path, *st, mode);  // just for mode
            IndexEntry e;
            e.path = sel.path;
            e.id = id;
            e.mode = mode;  // no stat cache: content differs from file
            next.add(e);
        } else {
            uint32_t m;
            if (st->is_link) {
                id = odb_->write(ObjType::Blob, worktree_content(sel.path));
                m = 0120000;
            } else {
                id = odb_->write(ObjType::Blob, to_odb(sel.path, read_file(p), true));
                m = (cfg_.get_bool("core.filemode", true) && st->is_exec) ? 0100755 : 0100644;
                if (!cfg_.get_bool("core.filemode", true))
                    if (auto* old = next.find(sel.path)) m = old->mode;
            }
            next.add(make_entry(sel.path, id, m));
        }
    }
    if (merging) {
        // all conflicts must be resolved and included
        for (auto& f : status_now.files)
            if (f.conflicted && std::none_of(files.begin(), files.end(), [&](auto& s) { return s.path == f.path; }))
                throw Error("Resolve and include " + f.path + " before committing the merge.");
    }
    ObjectId tree = next.write_tree(*odb_);
    std::string msg = trim(summary) + "\n";
    std::string desc = trim(description);
    if (!desc.empty() || !coauthors.empty()) msg += "\n";
    if (!desc.empty()) msg += desc + "\n";
    if (!coauthors.empty()) {
        if (!desc.empty()) msg += "\n";
        for (auto& [n, e] : coauthors) msg += "Co-authored-by: " + n + " <" + e + ">\n";
    }
    std::vector<ObjectId> parents;
    if (amend) {
        parents = odb_->read_commit(*h).parents;
    } else {
        if (h) parents.push_back(*h);
        if (merging && status_now.merge_head) parents.push_back(*status_now.merge_head);
        if (h && !merging) {
            auto ht = odb_->read_commit(*h).tree;
            if (ht == tree) throw Error("There are no changes to commit.");
        }
    }
    if (run_hooks) {
        // hooks see the index that is about to be committed
        auto saved = try_read_file(gitdir_ / "index");
        save_index(next);
        try {
            run_commit_hooks_pre(msg);
        } catch (...) {
            if (saved) write_file_atomic(gitdir_ / "index", *saved);
            throw;
        }
        next = load_index();  // a pre-commit hook may have staged more changes
        tree = next.write_tree(*odb_);
    }
    auto id = make_commit(tree, parents, msg,
                          amend ? "commit (amend): " : merging ? "commit (merge): " : h ? "commit: " : "commit (initial): ");
    // Keep the worktree versions of unselected changes in the worktree, but
    // the index now matches the new commit.
    save_index(next);
    if (merging) clear_merge_state();
    std::error_code ec;
    fs::remove(gitdir_ / "SQUASH_MSG", ec);
    if (run_hooks) run_post_commit_hook();
    return id;
}

void Repository::undo_last_commit() {
    auto h = head();
    if (!h) throw Error("no commits to undo");
    auto c = odb_->read_commit(*h);
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    Index idx = load_index();
    if (c.parents.empty()) {
        // undo the initial commit: unborn branch again
        auto br = refs_->head_target();
        refs_->remove(br);
        idx.entries.clear();
        save_index(idx);
        return;
    }
    refs_->update("HEAD", c.parents[0], "reset: moving to HEAD~1", who);
    idx.read_tree(*odb_, odb_->read_commit(c.parents[0]).tree);
    save_index(idx);
}

void Repository::discard(const std::vector<std::string>& paths) {
    auto st = status();
    std::map<std::string, StatusEntry> by;
    for (auto& f : st.files) by[f.path] = f;
    Index idx = load_index();
    auto h = head();
    std::optional<ObjectId> tree;
    if (h) tree = odb_->read_commit(*h).tree;
    for (auto& p : paths) {
        auto it = by.find(p);
        Change c = it == by.end() ? Change::Modified : it->second.change;
        if (c == Change::Untracked) {
            std::error_code ec;
            auto fp = wt_ / git_to_path(p.back() == '/' ? p.substr(0, p.size() - 1) : p);
            fs::remove_all(fp, ec);
            remove_worktree_file(p.back() == '/' ? p.substr(0, p.size() - 1) : p);
            continue;
        }
        auto restore = [&](const std::string& path) {
            std::optional<FlatEntry> e;
            if (tree) e = tree_entry(*tree, path);
            if (e) {
                write_worktree_file(path, odb_->read(e->id).data, e->mode);
                idx.add(make_entry(path, e->id, e->mode));
            } else {
                remove_worktree_file(path);
                idx.remove(path);
            }
        };
        restore(p);
        if (it != by.end() && !it->second.old_path.empty()) restore(it->second.old_path);
    }
    save_index(idx);
}

void Repository::discard_lines(const std::string& path, const std::vector<bool>& selection) {
    // Inverse of partial staging: selected changes are reverted in the worktree.
    Bytes old = head_content(path);
    Bytes cur = worktree_content(path);
    auto lines = full_diff_lines(old, cur);
    std::vector<bool> keep(lines.size(), true);
    for (auto& l : lines)
        if (l.index < int(selection.size()) && selection[l.index]) keep[l.index] = false;
    Bytes result = apply_selection(old, cur, keep);
    auto st = stat_path(wt_ / git_to_path(path));
    write_worktree_file(path, result, st && st->is_exec ? 0100755 : 0100644);
}

void Repository::add_to_gitignore(const std::string& pattern) {
    auto p = wt_ / ".gitignore";
    Bytes cur = try_read_file(p).value_or("");
    if (!cur.empty() && cur.back() != '\n') cur += '\n';
    cur += pattern + "\n";
    write_file(p, cur);
}

// ================================================================ history
static std::vector<std::string_view> blame_lines(const std::string& s) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) { out.push_back(std::string_view(s).substr(pos)); break; }
        out.push_back(std::string_view(s).substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

std::vector<BlameLine> Repository::blame(const std::string& path, const ObjectId& start, std::map<ObjectId, Commit>* commits) {
    ObjectId c = start;
    Commit cc = odb_->read_commit(c);
    if (commits) (*commits)[c] = cc;
    auto e = tree_entry(cc.tree, path);
    if (!e) throw Error(path + " doesn't exist in that commit");
    std::string final_text = blob(e->id);
    if (final_text.find('\0') != std::string::npos) throw Error(path + " is a binary file");
    auto final_lines = blame_lines(final_text);
    std::vector<BlameLine> out(final_lines.size());
    for (size_t i = 0; i < out.size(); i++) out[i].text = std::string(final_lines[i]);
    // map[i]: which final line the i-th line of the version we're looking at is (-1: already attributed)
    std::vector<int> map(final_lines.size());
    for (size_t i = 0; i < map.size(); i++) map[i] = int(i);
    std::string cur_text = final_text;
    ObjectId cur_blob = e->id;
    std::string cur_path = path;
    size_t remaining = map.size();
    auto assign_all = [&](const ObjectId& who) {
        for (size_t i = 0; i < map.size(); i++)
            if (map[i] >= 0) { out[size_t(map[i])].commit = who; out[size_t(map[i])].line = int(i) + 1; }
    };
    while (remaining) {
        if (cc.parents.empty()) { assign_all(c); break; }
        ObjectId p = cc.parents[0];
        Commit pc = odb_->read_commit(p);
        if (commits) (*commits)[p] = pc;
        auto pe = tree_entry(pc.tree, cur_path);
        if (!pe) {
            // renamed here? follow it when the parent has the same content under another name
            std::map<std::string, FlatEntry> flat;
            flatten_tree(*odb_, pc.tree, "", flat);
            for (auto& [p2, fe] : flat)
                if (fe.id == cur_blob && !tree_entry(cc.tree, p2)) { cur_path = p2; pe = fe; break; }
            if (!pe) { assign_all(c); break; }  // the file was added here
        }
        if (pe->id != cur_blob) {
            std::string ptext = blob(pe->id);
            auto A = blame_lines(ptext), B = blame_lines(cur_text);
            std::vector<int> next(A.size(), -1);
            for (auto& ed : myers(A, B)) {
                if (ed.op == Edit::Keep) next[size_t(ed.a)] = map[size_t(ed.b)];
                else if (ed.op == Edit::Ins && map[size_t(ed.b)] >= 0) {
                    out[size_t(map[size_t(ed.b)])].commit = c;
                    out[size_t(map[size_t(ed.b)])].line = ed.b + 1;
                    remaining--;
                }
            }
            map = std::move(next);
            cur_text = std::move(ptext);
            cur_blob = pe->id;
        }
        c = p;
        cc = std::move(pc);
    }
    return out;
}

std::vector<CommitInfo> Repository::search_log(const ObjectId& start, const std::string& query, size_t max, size_t scan_limit) {
    std::vector<CommitInfo> out;
    std::string q = to_lower(trim(query));
    if (q.empty()) return out;
    bool hexish = q.size() >= 4 && q.find_first_not_of("0123456789abcdef") == std::string::npos;
    struct Item { int64_t t; ObjectId id; bool operator<(const Item& o) const { return t < o.t; } };
    std::priority_queue<Item> pq;
    std::unordered_set<ObjectId, ObjectIdHash> seen;
    auto push = [&](const ObjectId& id) {
        if (!seen.insert(id).second) return;
        auto o = odb_->try_read(id);
        if (!o || o->type != ObjType::Commit) return;
        pq.push({Commit::parse(o->data).committer.time, id});
    };
    push(start);
    size_t scanned = 0;
    while (!pq.empty() && out.size() < max && scanned++ < scan_limit) {
        auto it = pq.top();
        pq.pop();
        auto c = odb_->read_commit(it.id);
        for (auto& p : c.parents) push(p);
        bool hit = (hexish && it.id.hex().rfind(q, 0) == 0) || to_lower(c.message).find(q) != std::string::npos ||
                   to_lower(c.author.name).find(q) != std::string::npos || to_lower(c.author.email).find(q) != std::string::npos;
        if (hit) out.push_back({it.id, std::move(c)});
    }
    return out;
}

std::vector<CommitInfo> Repository::log(const ObjectId& start, size_t max, size_t skip, const std::string& filter) {
    std::vector<CommitInfo> out;
    struct Item { int64_t t; ObjectId id; bool operator<(const Item& o) const { return t < o.t; } };
    std::priority_queue<Item> q;
    std::unordered_set<ObjectId, ObjectIdHash> seen;
    auto push = [&](const ObjectId& id) {
        if (!seen.insert(id).second) return;
        auto c = odb_->try_read(id);
        if (!c || c->type != ObjType::Commit) return;
        q.push({Commit::parse(c->data).committer.time, id});
    };
    push(start);
    size_t skipped = 0;
    while (!q.empty() && out.size() < max) {
        auto it = q.top();
        q.pop();
        auto c = odb_->read_commit(it.id);
        for (auto& p : c.parents) push(p);
        if (!filter.empty()) {
            auto mine = tree_entry(c.tree, filter);
            std::optional<FlatEntry> theirs;
            if (!c.parents.empty()) theirs = tree_entry(odb_->read_commit(c.parents[0]).tree, filter);
            bool changed = (mine.has_value() != theirs.has_value()) || (mine && theirs && mine->id != theirs->id);
            if (!changed) continue;
        }
        if (skipped < skip) { skipped++; continue; }
        out.push_back({it.id, std::move(c)});
    }
    return out;
}

std::vector<ChangedFile> Repository::tree_changes(const std::optional<ObjectId>& old_tree, const ObjectId& new_tree) {
    std::map<std::string, FlatEntry> a, b;
    if (old_tree) flatten_tree(*odb_, *old_tree, "", a);
    flatten_tree(*odb_, new_tree, "", b);
    std::vector<ChangedFile> out;
    for (auto& [p, e] : b) {
        auto it = a.find(p);
        if (it == a.end()) out.push_back({p, "", Change::Added, {}, e.id, 0, e.mode});
        else if (it->second.id != e.id || it->second.mode != e.mode)
            out.push_back({p, "", Change::Modified, it->second.id, e.id, it->second.mode, e.mode});
    }
    for (auto& [p, e] : a)
        if (!b.count(p)) out.push_back({p, "", Change::Deleted, e.id, {}, e.mode, 0});
    // exact renames
    std::unordered_map<ObjectId, size_t, ObjectIdHash> del;
    for (size_t i = 0; i < out.size(); i++) if (out[i].change == Change::Deleted) del[out[i].old_id] = i;
    std::vector<size_t> drop;
    for (auto& f : out) {
        if (f.change != Change::Added) continue;
        auto it = del.find(f.new_id);
        if (it == del.end()) continue;
        f.change = Change::Renamed;
        f.old_path = out[it->second].path;
        f.old_id = out[it->second].old_id;
        f.old_mode = out[it->second].old_mode;
        drop.push_back(it->second);
        del.erase(it);
    }
    std::sort(drop.rbegin(), drop.rend());
    for (auto i : drop) out.erase(out.begin() + long(i));
    std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.path < y.path; });
    return out;
}

std::vector<ChangedFile> Repository::commit_changes(const ObjectId& id) {
    auto c = odb_->read_commit(id);
    std::optional<ObjectId> parent_tree;
    if (!c.parents.empty()) parent_tree = odb_->read_commit(c.parents[0]).tree;
    return tree_changes(parent_tree, c.tree);
}

FileDiff Repository::commit_file_diff(const ChangedFile& f, int context, bool ignore_ws) {
    if (f.old_mode == 0160000 || f.new_mode == 0160000) {
        FileDiff d;
        return diff_text("Subproject commit " + f.old_id.hex() + "\n", "Subproject commit " + f.new_id.hex() + "\n");
    }
    Bytes a = f.old_id.is_zero() ? "" : smudged(f.old_path.empty() ? f.path : f.old_path, blob(f.old_id));
    Bytes b = f.new_id.is_zero() ? "" : smudged(f.path, blob(f.new_id));
    return diff_text(a, b, context, ignore_ws);
}

bool Repository::is_ancestor(const ObjectId& anc, const ObjectId& of) {
    if (anc == of) return true;
    auto base = merge_base(anc, of);
    return base && *base == anc;
}

std::optional<ObjectId> Repository::merge_base(const ObjectId& a, const ObjectId& b) {
    if (a == b) return a;
    // paint walk by commit time
    std::unordered_map<ObjectId, int, ObjectIdHash> flags;
    struct Item { int64_t t; ObjectId id; bool operator<(const Item& o) const { return t < o.t; } };
    std::priority_queue<Item> q;
    auto time_of = [&](const ObjectId& id) { return odb_->read_commit(id).committer.time; };
    flags[a] |= 1;
    flags[b] |= 2;
    q.push({time_of(a), a});
    q.push({time_of(b), b});
    std::vector<ObjectId> results;
    while (!q.empty()) {
        // stop when every queued commit is STALE (flag 4)
        bool all_stale = true;
        {
            auto copy = q;
            while (!copy.empty()) { if (!(flags[copy.top().id] & 4)) { all_stale = false; break; } copy.pop(); }
        }
        if (all_stale) break;
        auto it = q.top();
        q.pop();
        int f = flags[it.id];
        if ((f & 3) == 3 && !(f & 4)) {
            results.push_back(it.id);
            f |= 4;
            flags[it.id] = f;
        }
        auto c = odb_->read_commit(it.id);
        for (auto& p : c.parents) {
            int& pf = flags[p];
            int want = f & 7;
            if ((pf & want) == want) continue;
            pf |= want;
            q.push({time_of(p), p});
        }
    }
    if (results.empty()) return std::nullopt;
    // remove results that are ancestors of other results
    if (results.size() > 1) {
        for (size_t i = 0; i < results.size(); i++)
            for (size_t j = 0; j < results.size(); j++)
                if (i != j && results[i] != results[j]) {
                    auto bb = merge_base(results[i], results[j]);
                    if (bb && *bb == results[i]) { results.erase(results.begin() + long(i)); i--; break; }
                }
    }
    return results.front();
}

std::pair<int, int> Repository::ahead_behind(const ObjectId& local, const ObjectId& upstream) {
    if (local == upstream) return {0, 0};
    std::unordered_map<ObjectId, int, ObjectIdHash> flags;
    struct Item { int64_t t; ObjectId id; bool operator<(const Item& o) const { return t < o.t; } };
    std::priority_queue<Item> q;
    auto time_of = [&](const ObjectId& id) { return odb_->read_commit(id).committer.time; };
    flags[local] = 1;
    flags[upstream] |= 2;
    q.push({time_of(local), local});
    q.push({time_of(upstream), upstream});
    int ahead = 0, behind = 0;
    int pending_unique = 2;  // queued commits that are not yet in both
    while (!q.empty() && pending_unique > 0) {
        auto it = q.top();
        q.pop();
        int f = flags[it.id];
        if (f != 3) pending_unique--;
        if (f == 1) ahead++;
        else if (f == 2) behind++;
        auto c = odb_->read_commit(it.id);
        for (auto& p : c.parents) {
            auto found = flags.find(p);
            if (found == flags.end()) {
                flags[p] = f;
                q.push({time_of(p), p});
                if (f != 3) pending_unique++;
            } else if ((found->second | f) != found->second) {
                bool was_unique = found->second != 3;
                found->second |= f;
                if (was_unique && found->second == 3) pending_unique--;
            }
        }
    }
    return {ahead, behind};
}

std::optional<ObjectId> Repository::rev_parse(const std::string& spec) {
    if (auto colon = spec.find(':'); colon != std::string::npos && colon > 0) {  // rev:path
        auto c = rev_parse(spec.substr(0, colon));
        if (!c) return std::nullopt;
        auto e = tree_entry(odb_->read_commit(odb_->peel_to_commit(*c)).tree, spec.substr(colon + 1));
        if (!e) return std::nullopt;
        return e->id;
    }
    std::string s = spec;
    // suffixes ~N and ^N
    size_t cut = s.find_first_of("~^");
    std::string base = cut == std::string::npos ? s : s.substr(0, cut);
    std::optional<ObjectId> id;
    if (auto full = ObjectId::try_hex(base)) id = full;
    else if (auto r = refs_->dwim(base)) id = refs_->resolve(*r);
    else {
        auto m = odb_->resolve_prefix(base);
        if (m.size() == 1) id = m[0];
    }
    if (!id) return std::nullopt;
    size_t i = cut;
    while (i != std::string::npos && i < s.size()) {
        char op = s[i++];
        size_t j = i;
        while (j < s.size() && isdigit((unsigned char)s[j])) j++;
        int n = j > i ? std::stoi(s.substr(i, j - i)) : 1;
        i = j;
        auto cid = odb_->peel_to_commit(*id);
        if (op == '~') {
            for (int k = 0; k < n; k++) {
                auto c = odb_->read_commit(cid);
                if (c.parents.empty()) return std::nullopt;
                cid = c.parents[0];
            }
            id = cid;
        } else {
            if (n == 0) { id = cid; continue; }
            auto c = odb_->read_commit(cid);
            if (int(c.parents.size()) < n) return std::nullopt;
            id = c.parents[n - 1];
        }
    }
    return id;
}

// ================================================================ branches
std::string Repository::upstream_of(const std::string& branch) const {
    auto remote = cfg_.repo.get("branch." + branch + ".remote");
    auto merge = cfg_.repo.get("branch." + branch + ".merge");
    if (!remote || !merge || *remote == ".") return "";
    if (starts_with(*merge, "refs/heads/")) return "refs/remotes/" + *remote + "/" + merge->substr(11);
    return "";
}

void Repository::set_upstream(const std::string& branch, const std::string& remote, const std::string& merge_ref) {
    cfg_.repo.set("branch." + branch + ".remote", remote);
    cfg_.repo.set("branch." + branch + ".merge", merge_ref);
    cfg_.repo.save();
}

std::vector<BranchInfo> Repository::branches(bool include_remote) {
    std::vector<BranchInfo> out;
    auto cur = refs_->head_target();
    for (auto& r : refs_->list("refs/heads/")) {
        BranchInfo b;
        b.full = r.name;
        b.name = r.name.substr(11);
        b.tip = r.target;
        b.current = r.name == cur;
        b.upstream = upstream_of(b.name);
        try {
            auto c = odb_->read_commit(r.target);
            b.tip_time = c.committer.time;
            b.tip_summary = c.summary();
        } catch (...) {}
        if (!b.upstream.empty())
            if (auto u = refs_->resolve(b.upstream)) {
                try { std::tie(b.ahead, b.behind) = ahead_behind(b.tip, *u); } catch (...) {}
            }
        out.push_back(b);
    }
    if (include_remote) {
        for (auto& r : refs_->list("refs/remotes/")) {
            if (r.name.size() > 5 && r.name.substr(r.name.size() - 5) == "/HEAD") continue;
            BranchInfo b;
            b.full = r.name;
            b.name = r.name.substr(13);
            b.tip = r.target;
            b.remote = true;
            try {
                auto c = odb_->read_commit(r.target);
                b.tip_time = c.committer.time;
                b.tip_summary = c.summary();
            } catch (...) {}
            out.push_back(b);
        }
    }
    return out;
}

void Repository::switch_tree(const std::optional<ObjectId>& from_tree, const ObjectId& to_tree, bool force) {
    std::map<std::string, FlatEntry> a, b;
    if (from_tree) flatten_tree(*odb_, *from_tree, "", a);
    flatten_tree(*odb_, to_tree, "", b);
    Index idx = load_index();
    // determine dirty paths (worktree or index differs from HEAD)
    std::set<std::string> dirty;
    if (!force) {
        auto st = status();
        for (auto& f : st.files) {
            if (f.conflicted) throw Error("Resolve the conflicts in your working directory first.");
            dirty.insert(f.path);
            if (!f.old_path.empty()) dirty.insert(f.old_path);
        }
    }
    std::vector<std::string> clobbered;
    std::set<std::string> touched;
    for (auto& [p, e] : b) {
        auto it = a.find(p);
        if (it != a.end() && it->second.id == e.id && it->second.mode == e.mode) continue;
        touched.insert(p);
    }
    for (auto& [p, e] : a) if (!b.count(p)) touched.insert(p);
    for (auto& p : touched) if (dirty.count(p)) clobbered.push_back(p);
    if (!clobbered.empty()) {
        std::string msg = "Your local changes to the following files would be overwritten:\n";
        for (size_t i = 0; i < clobbered.size() && i < 10; i++) msg += "  " + clobbered[i] + "\n";
        msg += "Commit or stash your changes first.";
        throw Error(msg);
    }
    for (auto& p : touched) {
        auto it = b.find(p);
        if (it == b.end()) {
            remove_worktree_file(p);
            idx.remove(p);
        } else if (it->second.mode == 0160000) {
            std::error_code ec;
            fs::create_directories(wt_ / git_to_path(p), ec);
            IndexEntry e;
            e.path = p; e.id = it->second.id; e.mode = 0160000;
            idx.add(e);
        } else {
            write_worktree_file(p, odb_->read(it->second.id).data, it->second.mode);
            idx.add(make_entry(p, it->second.id, it->second.mode));
        }
    }
    // entries for untouched paths: keep the index's staged state unless force
    if (force) {
        Index fresh;
        fresh.read_tree(*odb_, to_tree);
        for (auto& e : fresh.entries) {
            auto* old = idx.find(e.path);
            if (old && old->id == e.id) e.st = old->st;
            else {
                write_worktree_file(e.path, odb_->read(e.id).data, e.mode);
                e = make_entry(e.path, e.id, e.mode);
            }
        }
        // delete tracked files that vanished
        for (auto& e : idx.entries) if (!fresh.find(e.path)) remove_worktree_file(e.path);
        idx = std::move(fresh);
    }
    save_index(idx);
}

void Repository::create_branch(const std::string& name, const std::optional<ObjectId>& start, bool checkout) {
    if (!is_valid_branch_name(name)) throw Error("'" + name + "' is not a valid branch name.");
    std::string full = "refs/heads/" + name;
    if (refs_->resolve(full)) throw Error("A branch named " + name + " already exists.");
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    auto from = start ? start : head();
    if (!from) {
        // unborn: just repoint HEAD
        if (checkout) refs_->set_symbolic("HEAD", full);
        return;
    }
    refs_->update(full, *from, "branch: Created from " + (start ? start->short_hex() : std::string("HEAD")), who);
    if (checkout) checkout_branch(name);
}

void Repository::checkout_branch(const std::string& name) {
    std::string full = "refs/heads/" + name;
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    if (!refs_->resolve(full)) {
        // remote branch: create a tracking branch
        if (auto rid = refs_->resolve("refs/remotes/" + name)) {
            size_t slash = name.find('/');
            std::string remote = name.substr(0, slash), bname = name.substr(slash + 1);
            if (refs_->resolve("refs/heads/" + bname)) {
                checkout_branch(bname);
                return;
            }
            auto from_tree = head() ? std::optional(odb_->read_commit(*head()).tree) : std::nullopt;
            switch_tree(from_tree, odb_->read_commit(*rid).tree);
            refs_->update("refs/heads/" + bname, *rid, "branch: Created from " + name, who);
            set_upstream(bname, remote, "refs/heads/" + bname);
            auto old = head().value_or(ObjectId{});
            refs_->set_symbolic("HEAD", "refs/heads/" + bname);
            refs_->append_reflog("HEAD", old, *rid, who, "checkout: moving to " + bname);
            return;
        }
        throw Error("no branch named " + name);
    }
    auto target = *refs_->resolve(full);
    auto old = head();
    auto from_tree = old ? std::optional(odb_->read_commit(*old).tree) : std::nullopt;
    switch_tree(from_tree, odb_->read_commit(target).tree);
    std::string from_name = current_branch().empty() ? (old ? old->short_hex() : "") : current_branch();
    refs_->set_symbolic("HEAD", full);
    refs_->append_reflog("HEAD", old.value_or(ObjectId{}), target, who,
                         "checkout: moving from " + from_name + " to " + name);
}

void Repository::checkout_commit(const ObjectId& id) {
    auto old = head();
    auto from_tree = old ? std::optional(odb_->read_commit(*old).tree) : std::nullopt;
    switch_tree(from_tree, odb_->read_commit(id).tree);
    write_file(gitdir_ / "HEAD", id.hex() + "\n");
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    refs_->append_reflog("HEAD", old.value_or(ObjectId{}), id, who, "checkout: moving to " + id.hex());
}

void Repository::delete_branch(const std::string& name, bool also_tracking) {
    if (current_branch() == name) throw Error("You can't delete the branch you're on.");
    std::string up = upstream_of(name);
    refs_->remove("refs/heads/" + name);
    cfg_.repo.remove_section("branch", name);
    cfg_.repo.save();
    if (also_tracking && !up.empty()) refs_->remove(up);
}

void Repository::rename_branch(const std::string& from, const std::string& to) {
    if (!is_valid_branch_name(to)) throw Error("'" + to + "' is not a valid branch name.");
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    if (current_branch() == from && !refs_->resolve("refs/heads/" + from)) {
        refs_->set_symbolic("HEAD", "refs/heads/" + to);  // unborn
        return;
    }
    refs_->rename("refs/heads/" + from, "refs/heads/" + to, who);
    cfg_.repo.rename_section("branch", from, to);
    cfg_.repo.save();
}

// ================================================================ merge
void Repository::write_merge_state(const ObjectId& other, const std::string& msg) {
    write_file(gitdir_ / "MERGE_HEAD", other.hex() + "\n");
    write_file(gitdir_ / "MERGE_MSG", msg);
    write_file(gitdir_ / "MERGE_MODE", "");
}

void Repository::clear_merge_state() {
    std::error_code ec;
    for (auto f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "ORIG_HEAD_MERGE"}) fs::remove(gitdir_ / f, ec);
}

MergeOutcome Repository::three_way(const ObjectId& base_tree, const ObjectId& ours_tree, const ObjectId& theirs_tree,
                                   const std::string& ours_label, const std::string& theirs_label, Index& idx,
                                   std::vector<std::string>& conflicts, bool dry) {
    std::map<std::string, FlatEntry> B, O, T;
    if (!base_tree.is_zero()) flatten_tree(*odb_, base_tree, "", B);
    flatten_tree(*odb_, ours_tree, "", O);
    flatten_tree(*odb_, theirs_tree, "", T);
    std::set<std::string> paths;
    for (auto* m : {&B, &O, &T}) for (auto& [p, _] : *m) paths.insert(p);
    auto get = [](std::map<std::string, FlatEntry>& m, const std::string& p) -> std::optional<FlatEntry> {
        auto it = m.find(p);
        if (it == m.end()) return std::nullopt;
        return it->second;
    };
    auto eq = [](const std::optional<FlatEntry>& x, const std::optional<FlatEntry>& y) {
        if (x.has_value() != y.has_value()) return false;
        return !x || (x->id == y->id && x->mode == y->mode);
    };
    for (auto& p : paths) {
        auto b = get(B, p), o = get(O, p), t = get(T, p);
        if (eq(o, t) || eq(b, t)) continue;  // nothing to do (ours stays)
        if (eq(b, o)) {
            if (dry) continue;
            // take theirs
            if (t) {
                write_worktree_file(p, odb_->read(t->id).data, t->mode);
                idx.add(make_entry(p, t->id, t->mode));
            } else {
                remove_worktree_file(p);
                idx.remove(p);
            }
            continue;
        }
        // both changed
        if (o && t && b && o->mode != 0160000 && t->mode != 0160000) {
            Bytes bd = odb_->read(b->id).data, od = odb_->read(o->id).data, td = odb_->read(t->id).data;
            if (!looks_binary(bd) && !looks_binary(od) && !looks_binary(td)) {
                auto mr = merge3(bd, od, td, ours_label, theirs_label);
                uint32_t mode = o->mode == b->mode ? t->mode : o->mode;
                if (!mr.conflict) {
                    if (dry) continue;
                    auto id = odb_->write(ObjType::Blob, mr.text);
                    write_worktree_file(p, mr.text, mode);
                    idx.add(make_entry(p, id, mode));
                    continue;
                }
                if (!dry) write_worktree_file(p, mr.text, mode);
            }
        } else if (o && t && !b) {
            // add/add
            Bytes od = odb_->read(o->id).data, td = odb_->read(t->id).data;
            if (!looks_binary(od) && !looks_binary(td) && !dry) {
                auto mr = merge3("", od, td, ours_label, theirs_label);
                write_worktree_file(p, mr.text, o->mode);
            }
        } else if (!o && t) {
            if (!dry) write_worktree_file(p, odb_->read(t->id).data, t->mode);  // deleted by us
        }
        conflicts.push_back(p);
        if (dry) continue;
        idx.remove(p);
        auto add_stage = [&](const std::optional<FlatEntry>& e, int stage) {
            if (!e) return;
            IndexEntry ie;
            ie.path = p; ie.id = e->id; ie.mode = e->mode; ie.stage = stage;
            idx.entries.push_back(ie);
        };
        add_stage(b, 1);
        add_stage(o, 2);
        add_stage(t, 3);
        idx.sort();
    }
    return {conflicts.empty() ? MergeOutcome::Merged : MergeOutcome::Conflicts, conflicts, std::nullopt};
}

MergeOutcome Repository::merge(const std::string& branch_ref, bool squash) {
    auto h = head();
    auto other = refs_->resolve(branch_ref);
    if (!other) throw Error("no such branch " + branch_ref);
    *other = odb_->peel_to_commit(*other);
    auto who = signature();
    if (!h) {
        // unborn: fast-forward
        switch_tree(std::nullopt, odb_->read_commit(*other).tree);
        refs_->update("HEAD", *other, "merge " + shorten_ref(branch_ref) + ": Fast-forward", who);
        return {MergeOutcome::FastForward, {}, *other};
    }
    if (is_ancestor(*other, *h)) return {MergeOutcome::UpToDate, {}, std::nullopt};
    auto ht = odb_->read_commit(*h).tree;
    if (is_ancestor(*h, *other)) {
        switch_tree(ht, odb_->read_commit(*other).tree);
        refs_->update("HEAD", *other, "merge " + shorten_ref(branch_ref) + ": Fast-forward", who);
        return {MergeOutcome::FastForward, {}, *other};
    }
    // require a clean tree for tracked files
    for (auto& f : status().files)
        if (f.change != Change::Untracked)
            throw Error("You have uncommitted changes. Commit or stash them before merging.");
    auto base = merge_base(*h, *other);
    ObjectId base_tree = base ? odb_->read_commit(*base).tree : ObjectId{};
    Index idx = load_index();
    std::vector<std::string> conflicts;
    std::string cur = current_branch().empty() ? "HEAD" : current_branch();
    std::string label = shorten_ref(branch_ref);
    three_way(base_tree, ht, odb_->read_commit(*other).tree, "HEAD", label, idx, conflicts);
    save_index(idx);
    std::string msg = "Merge " + std::string(starts_with(branch_ref, "refs/remotes/") ? "remote-tracking branch '" : "branch '") +
                      label + "' into " + cur + "\n";
    if (squash) {
        // single-parent commit containing all of the branch's changes
        msg = "Squashed commit of '" + label + "'\n\n";
        if (base)
            for (auto& id : commits_between(*base, *other)) msg += "* " + odb_->read_commit(id).summary() + "\n";
        if (!conflicts.empty()) {
            write_file(gitdir_ / "SQUASH_MSG", msg);
            write_file(gitdir_ / "MERGE_MSG", msg);
            return {MergeOutcome::Conflicts, conflicts, std::nullopt};
        }
        auto tree = idx.write_tree(*odb_);
        if (tree == ht) return {MergeOutcome::UpToDate, {}, std::nullopt};
        auto id = make_commit(tree, {*h}, msg, "merge --squash " + label + ": ");
        return {MergeOutcome::Merged, {}, id};
    }
    if (!conflicts.empty()) {
        write_merge_state(*other, msg);
        return {MergeOutcome::Conflicts, conflicts, std::nullopt};
    }
    auto tree = idx.write_tree(*odb_);
    auto id = make_commit(tree, {*h, *other}, msg, "merge " + label + ": ");
    return {MergeOutcome::Merged, {}, id};
}

bool Repository::squash_in_progress() const { return fs::exists(gitdir_ / "SQUASH_MSG"); }

MergePreview Repository::merge_preview(const std::string& branch_ref) {
    MergePreview p;
    auto h = head();
    auto other = refs_->resolve(branch_ref);
    if (!other) return p;
    *other = odb_->peel_to_commit(*other);
    if (!h) { p.kind = MergePreview::FastForward; return p; }
    if (is_ancestor(*other, *h)) return p;
    auto base = merge_base(*h, *other);
    if (base) p.commits = int(commits_between(*base, *other).size());
    if (is_ancestor(*h, *other)) { p.kind = MergePreview::FastForward; return p; }
    if (!base) { p.kind = MergePreview::Unrelated; return p; }
    Index scratch;
    std::vector<std::string> conflicts;
    three_way(odb_->read_commit(*base).tree, odb_->read_commit(*h).tree, odb_->read_commit(*other).tree, "", "", scratch,
              conflicts, true);
    p.conflicts = int(conflicts.size());
    p.kind = conflicts.empty() ? MergePreview::Clean : MergePreview::Conflicts;
    return p;
}

void Repository::abort_merge() {
    auto h = head();
    if (h) reset_hard(*h);
    clear_merge_state();
}

void Repository::resolve_with(const std::string& path, int stage) {
    Index idx = load_index();
    const IndexEntry* e = idx.find(path, stage);
    if (!e) {
        // that side deleted the file
        remove_worktree_file(path);
        idx.remove(path);
    } else {
        auto id = e->id;
        auto mode = e->mode;
        write_worktree_file(path, odb_->read(id).data, mode);
        idx.remove(path);
        idx.add(make_entry(path, id, mode));
    }
    save_index(idx);
}

void Repository::reset_hard(const ObjectId& to) {
    auto tree = odb_->read_commit(to).tree;
    Index idx = load_index();
    std::map<std::string, FlatEntry> target;
    flatten_tree(*odb_, tree, "", target);
    // remove files that are tracked now but not in target
    for (auto& e : idx.entries) if (!target.count(e.path)) remove_worktree_file(e.path);
    Index fresh;
    for (auto& [p, fe] : target) {
        write_worktree_file(p, odb_->read(fe.id).data, fe.mode);
        fresh.entries.push_back(make_entry(p, fe.id, fe.mode));
    }
    fresh.sort();
    save_index(fresh);
    if (head() != to) {
        auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
        refs_->update("HEAD", to, "reset: moving to " + to.hex(), who);
    }
}

ObjectId Repository::revert(const ObjectId& cid) {
    auto h = head();
    if (!h) throw Error("nothing to revert onto");
    for (auto& f : status().files)
        if (f.change != Change::Untracked) throw Error("Commit or stash your changes before reverting.");
    auto c = odb_->read_commit(cid);
    if (c.parents.size() > 1) throw Error("Reverting merge commits isn't supported yet.");
    ObjectId parent_tree = c.parents.empty() ? odb_->write(ObjType::Tree, "") : odb_->read_commit(c.parents[0]).tree;
    Index idx = load_index();
    std::vector<std::string> conflicts;
    three_way(c.tree, odb_->read_commit(*h).tree, parent_tree, "HEAD", "parent of " + cid.short_hex(), idx, conflicts);
    save_index(idx);
    std::string msg = "Revert \"" + c.summary() + "\"\n\nThis reverts commit " + cid.hex() + ".\n";
    if (!conflicts.empty()) {
        write_file(gitdir_ / "MERGE_MSG", msg);
        throw Error("Reverting caused conflicts in " + std::to_string(conflicts.size()) +
                    " file(s). Resolve them, then commit.");
    }
    return make_commit(idx.write_tree(*odb_), {*h}, msg, "revert: ");
}

ObjectId Repository::cherry_pick(const ObjectId& cid) {
    auto h = head();
    if (!h) throw Error("nothing to cherry-pick onto");
    auto c = odb_->read_commit(cid);
    ObjectId parent_tree = c.parents.empty() ? odb_->write(ObjType::Tree, "") : odb_->read_commit(c.parents[0]).tree;
    Index idx = load_index();
    std::vector<std::string> conflicts;
    three_way(parent_tree, odb_->read_commit(*h).tree, c.tree, "HEAD", cid.short_hex(), idx, conflicts);
    save_index(idx);
    if (!conflicts.empty()) throw Error("Cherry-pick caused conflicts. Resolve them, then commit.");
    Commit nc = c;
    nc.parents = {*h};
    nc.tree = idx.write_tree(*odb_);
    nc.committer = signature();
    auto id = write_commit(nc);
    refs_->update("HEAD", id, "cherry-pick: " + c.summary(), nc.committer);
    return id;
}

// ================================================================ stash
std::vector<StashInfo> Repository::stashes() {
    std::vector<StashInfo> out;
    auto log = refs_->reflog("refs/stash");
    int n = int(log.size());
    for (int i = n - 1; i >= 0; i--) {
        StashInfo s;
        s.index = n - 1 - i;
        s.id = log[i].new_id;
        s.message = log[i].message;
        s.time = log[i].who.time;
        try {
            auto c = odb_->read_commit(s.id);
            if (!c.parents.empty()) s.base = c.parents[0];
        } catch (...) { continue; }
        // "On main: message" / "WIP on main: ..."
        auto colon = s.message.find(':');
        if (colon != std::string::npos) {
            auto pre = s.message.substr(0, colon);
            auto on = pre.rfind("on ");
            if (on != std::string::npos) s.branch = pre.substr(on + 3);
        }
        out.push_back(s);
    }
    return out;
}

ObjectId Repository::stash_push(const std::string& message, bool include_untracked) {
    auto h = head();
    if (!h) throw Error("You need at least one commit before you can stash.");
    auto st = status();
    if (st.files.empty()) throw Error("There are no changes to stash.");
    auto who = signature();
    auto hc = odb_->read_commit(*h);
    Index idx = load_index();
    std::string branch = current_branch().empty() ? "(no branch)" : current_branch();
    std::string msg = message.empty() ? "WIP on " + branch + ": " + h->short_hex() + " " + hc.summary()
                                      : "On " + branch + ": " + message;
    // index commit
    Commit ic;
    ic.tree = idx.write_tree(*odb_);
    ic.parents = {*h};
    ic.author = ic.committer = who;
    ic.message = "index on " + branch + ": " + h->short_hex() + " " + hc.summary() + "\n";
    auto iid = odb_->write(ObjType::Commit, ic.serialize());
    // worktree tree: index + tracked worktree changes
    Index w = idx;
    std::vector<std::string> untracked;
    for (auto& f : st.files) {
        if (f.change == Change::Untracked) { if (f.path.back() != '/') untracked.push_back(f.path); continue; }
        if (!f.old_path.empty()) w.remove(f.old_path);
        auto stt = stat_path(wt_ / git_to_path(f.path));
        if (!stt) { w.remove(f.path); continue; }
        uint32_t mode;
        hash_worktree_file(f.path, *stt, mode);
        auto id = odb_->write(ObjType::Blob, worktree_blob(f.path));
        IndexEntry e; e.path = f.path; e.id = id; e.mode = mode;
        w.add(e);
    }
    for (auto& f : st.files) if (f.change == Change::Renamed) {
        auto stt = stat_path(wt_ / git_to_path(f.path));
        if (!stt) continue;
        uint32_t mode;
        hash_worktree_file(f.path, *stt, mode);
        IndexEntry e; e.path = f.path; e.id = odb_->write(ObjType::Blob, worktree_blob(f.path)); e.mode = mode;
        w.add(e);
    }
    Commit wc;
    wc.tree = w.write_tree(*odb_);
    wc.parents = {*h, iid};
    if (include_untracked && !untracked.empty()) {
        Index u;
        for (auto& p : untracked) {
            auto stt = stat_path(wt_ / git_to_path(p));
            if (!stt) continue;
            uint32_t mode;
            hash_worktree_file(p, *stt, mode);
            IndexEntry e; e.path = p; e.id = odb_->write(ObjType::Blob, worktree_blob(p)); e.mode = mode;
            u.add(e);
        }
        Commit uc;
        uc.tree = u.write_tree(*odb_);
        uc.author = uc.committer = who;
        uc.message = "untracked files on " + branch + ": " + h->short_hex() + " " + hc.summary() + "\n";
        wc.parents.push_back(odb_->write(ObjType::Commit, uc.serialize()));
    }
    wc.author = wc.committer = who;
    wc.message = msg + "\n";
    auto wid = odb_->write(ObjType::Commit, wc.serialize());
    auto old = refs_->resolve("refs/stash").value_or(ObjectId{});
    {
        LockFile lf(gitdir_ / "refs" / "stash");
        lf.write(wid.hex() + "\n");
        lf.commit();
    }
    refs_->append_reflog("refs/stash", old, wid, who, msg);
    // clean the worktree back to HEAD
    reset_hard(*h);
    if (include_untracked)
        for (auto& p : untracked) remove_worktree_file(p);
    return wid;
}

void Repository::stash_apply(int index, bool pop) {
    auto all = stashes();
    auto it = std::find_if(all.begin(), all.end(), [&](auto& s) { return s.index == index; });
    if (it == all.end()) throw Error("no such stash");
    auto wc = odb_->read_commit(it->id);
    auto h = head();
    if (!h) throw Error("no HEAD");
    // untracked files must not already exist
    std::map<std::string, FlatEntry> untracked;
    if (wc.parents.size() >= 3) flatten_tree(*odb_, odb_->read_commit(wc.parents[2]).tree, "", untracked);
    for (auto& [p, _] : untracked)
        if (fs::exists(wt_ / git_to_path(p))) throw Error(p + " already exists; can't restore the stash.");
    auto base_tree = odb_->read_commit(wc.parents[0]).tree;
    auto head_tree = odb_->read_commit(*h).tree;
    // tracked changes must not overlap with current local changes
    auto st = status();
    std::set<std::string> dirty;
    for (auto& f : st.files) if (f.change != Change::Untracked) dirty.insert(f.path);
    auto changes = tree_changes(base_tree, wc.tree);
    for (auto& c : changes)
        if (dirty.count(c.path)) throw Error("Your local changes to " + c.path + " conflict with the stash.");
    Index idx = load_index();
    std::vector<std::string> conflicts;
    if (base_tree == head_tree) {
        for (auto& c : changes) {
            if (c.change == Change::Deleted) remove_worktree_file(c.path);
            else {
                write_worktree_file(c.path, odb_->read(c.new_id).data, c.new_mode);
                if (c.change == Change::Renamed) remove_worktree_file(c.old_path);
            }
        }
    } else {
        // apply as a 3-way merge but keep results as unstaged changes
        Index scratch = idx;
        three_way(base_tree, head_tree, wc.tree, "Updated upstream", "Stashed changes", scratch, conflicts);
        if (!conflicts.empty()) {
            save_index(scratch);
            throw Error("Restoring the stash caused conflicts. Resolve them before continuing.");
        }
    }
    for (auto& [p, fe] : untracked) write_worktree_file(p, odb_->read(fe.id).data, fe.mode);
    if (pop) stash_drop(index);
}

void Repository::stash_drop(int index) {
    auto log = refs_->reflog("refs/stash");
    int n = int(log.size());
    int pos = n - 1 - index;
    if (pos < 0 || pos >= n) throw Error("no such stash");
    log.erase(log.begin() + pos);
    if (log.empty()) {
        refs_->remove("refs/stash");
        return;
    }
    refs_->write_reflog("refs/stash", log);
    LockFile lf(gitdir_ / "refs" / "stash");
    lf.write(log.back().new_id.hex() + "\n");
    lf.commit();
}

std::vector<ChangedFile> Repository::stash_changes(int index) {
    auto all = stashes();
    for (auto& s : all) {
        if (s.index != index) continue;
        auto wc = odb_->read_commit(s.id);
        auto out = tree_changes(odb_->read_commit(wc.parents[0]).tree, wc.tree);
        if (wc.parents.size() >= 3) {
            for (auto& u : tree_changes(std::nullopt, odb_->read_commit(wc.parents[2]).tree)) out.push_back(u);
        }
        return out;
    }
    return {};
}

// ================================================================ remotes, tags
std::vector<Remote> Repository::remotes() const {
    std::vector<Remote> out;
    for (auto& name : cfg_.repo.subsections("remote")) {
        Remote r;
        r.name = name;
        r.url = cfg_.repo.get("remote." + name + ".url").value_or("");
        r.push_url = cfg_.repo.get("remote." + name + ".pushurl").value_or(r.url);
        if (!r.url.empty()) out.push_back(r);
    }
    return out;
}

void Repository::add_remote(const std::string& name, const std::string& url) {
    if (cfg_.repo.get("remote." + name + ".url")) throw Error("remote " + name + " already exists");
    cfg_.repo.set("remote." + name + ".url", url);
    cfg_.repo.set("remote." + name + ".fetch", "+refs/heads/*:refs/remotes/" + name + "/*");
    cfg_.repo.save();
}

void Repository::set_remote_url(const std::string& name, const std::string& url) {
    cfg_.repo.set("remote." + name + ".url", url);
    cfg_.repo.save();
}

void Repository::remove_remote(const std::string& name) {
    cfg_.repo.remove_section("remote", name);
    cfg_.repo.save();
    for (auto& r : refs_->list("refs/remotes/" + name + "/")) refs_->remove(r.name);
}

std::vector<std::pair<std::string, ObjectId>> Repository::tags() {
    std::vector<std::pair<std::string, ObjectId>> out;
    for (auto& r : refs_->list("refs/tags/")) out.push_back({r.name.substr(10), r.target});
    return out;
}

void Repository::create_tag(const std::string& name, const ObjectId& target, const std::string& message) {
    if (!valid_ref_name("refs/tags/" + name)) throw Error("invalid tag name");
    if (refs_->resolve("refs/tags/" + name)) throw Error("tag " + name + " already exists");
    ObjectId id = target;
    auto who = signature();
    if (!message.empty()) {
        Tag t;
        t.object = target; t.type = ObjType::Commit; t.name = name; t.tagger = who; t.message = message + "\n";
        id = odb_->write(ObjType::Tag, t.serialize());
    }
    LockFile lf(gitdir_ / "refs" / "tags" / git_to_path(name));
    lf.write(id.hex() + "\n");
    lf.commit();
}

std::vector<ObjectId> Repository::local_commit_tips() {
    std::vector<ObjectId> out;
    for (auto& prefix : {"refs/heads/", "refs/remotes/", "refs/tags/"})
        for (auto& r : refs_->list(prefix)) out.push_back(r.target);
    if (auto h = head()) out.push_back(*h);
    return out;
}

std::set<ObjectId> Repository::reachable_objects(const std::vector<ObjectId>& tips, const std::set<ObjectId>& stop) {
    // objects reachable from `tips` but not from `stop` commits
    std::set<ObjectId> excluded_commits, excluded_trees_blobs;
    {
        std::deque<ObjectId> q;
        for (auto& s : stop) if (odb_->exists(s)) q.push_back(s);
        // exclude stop commits and their ancestry (bounded) plus their trees
        size_t budget = 20000;
        while (!q.empty() && budget--) {
            auto id = q.front(); q.pop_front();
            if (!excluded_commits.insert(id).second) continue;
            auto o = odb_->try_read(id);
            if (!o || o->type != ObjType::Commit) continue;
            auto c = Commit::parse(o->data);
            for (auto& p : c.parents) q.push_back(p);
        }
        // trees/blobs of the stop tips themselves (not full history, for speed)
        std::function<void(const ObjectId&)> mark = [&](const ObjectId& tree) {
            if (!excluded_trees_blobs.insert(tree).second) return;
            auto t = odb_->read_tree(tree);
            for (auto& e : t.entries) {
                if (e.is_submodule()) continue;
                if (e.is_tree()) mark(e.id);
                else excluded_trees_blobs.insert(e.id);
            }
        };
        for (auto& s : stop) {
            auto o = odb_->try_read(s);
            if (o && o->type == ObjType::Commit) mark(Commit::parse(o->data).tree);
        }
    }
    std::set<ObjectId> out;
    std::deque<ObjectId> q(tips.begin(), tips.end());
    std::function<void(const ObjectId&)> add_tree = [&](const ObjectId& tree) {
        if (excluded_trees_blobs.count(tree) || !out.insert(tree).second) return;
        auto t = odb_->read_tree(tree);
        for (auto& e : t.entries) {
            if (e.is_submodule()) continue;
            if (e.is_tree()) add_tree(e.id);
            else if (!excluded_trees_blobs.count(e.id)) out.insert(e.id);
        }
    };
    while (!q.empty()) {
        auto id = q.front(); q.pop_front();
        if (excluded_commits.count(id) || out.count(id)) continue;
        auto o = odb_->read(id);
        if (o.type == ObjType::Tag) { out.insert(id); q.push_back(Tag::parse(o.data).object); continue; }
        if (o.type != ObjType::Commit) continue;
        out.insert(id);
        auto c = Commit::parse(o.data);
        add_tree(c.tree);
        for (auto& p : c.parents) q.push_back(p);
    }
    return out;
}

// ================================================================ hooks
fs::path Repository::hooks_dir() const {
    if (auto hp = cfg_.get("core.hookspath")) {
        std::string h = *hp;
        if (starts_with(h, "~/")) return home_dir() / git_to_path(h.substr(2));
        fs::path p = git_to_path(h);
        return p.is_relative() ? wt_ / p : p;
    }
    return gitdir_ / "hooks";
}

void Repository::run_commit_hooks_pre(std::string& message) {
    if (auto r = run_hook(hooks_dir(), gitdir_, wt_, "pre-commit"); r && r->exit_code != 0)
        throw Error("The pre-commit hook failed:\n\n" + trim(r->output));
    auto msg_file = gitdir_ / "COMMIT_EDITMSG";
    write_file(msg_file, message);
    if (auto r = run_hook(hooks_dir(), gitdir_, wt_, "prepare-commit-msg", {path_to_git(msg_file), "message"}); r && r->exit_code != 0)
        throw Error("The prepare-commit-msg hook failed:\n\n" + trim(r->output));
    if (auto r = run_hook(hooks_dir(), gitdir_, wt_, "commit-msg", {path_to_git(msg_file)}); r && r->exit_code != 0)
        throw Error("The commit-msg hook failed:\n\n" + trim(r->output));
    // hooks may rewrite the message
    if (auto m = try_read_file(msg_file)) {
        std::string cleaned;
        for (auto line : split_lines(*m))
            if (line.empty() || line[0] != '#') { cleaned += line; cleaned += '\n'; }
        if (!trim(cleaned).empty()) message = trim(cleaned) + "\n";
    }
}

void Repository::run_post_commit_hook() { run_hook(hooks_dir(), gitdir_, wt_, "post-commit"); }

// ================================================================ resets, tags
void Repository::reset_mixed(const ObjectId& to) {
    auto who = Signature::now(user_name().value_or("Tributary"), user_email().value_or(""));
    if (auto h = head()) write_file(gitdir_ / "ORIG_HEAD", h->hex() + "\n");
    refs_->update("HEAD", to, "reset: moving to " + to.hex(), who);
    Index idx = load_index();
    idx.read_tree(*odb_, odb_->read_commit(to).tree);
    save_index(idx);
}

void Repository::delete_tag(const std::string& name) {
    if (!refs_->resolve("refs/tags/" + name)) throw Error("no tag named " + name);
    refs_->remove("refs/tags/" + name);
}

// ================================================================ rewriting
std::vector<ObjectId> Repository::commits_between(const ObjectId& base, const ObjectId& tip) {
    // commits reachable from tip but not from base, newest first (date order)
    std::vector<ObjectId> out;
    std::set<ObjectId> excluded;
    if (!base.is_zero()) {
        std::deque<ObjectId> q{base};
        size_t budget = 200000;
        while (!q.empty() && budget--) {
            auto id = q.front();
            q.pop_front();
            if (!excluded.insert(id).second) continue;
            for (auto& p : odb_->read_commit(id).parents) q.push_back(p);
        }
    }
    for (auto& c : log(tip, 100000)) {
        if (excluded.count(c.id)) continue;
        out.push_back(c.id);
    }
    return out;
}

static const char* op_name(RewriteStep::Op o) {
    switch (o) {
        case RewriteStep::Pick: return "pick";
        case RewriteStep::Reword: return "reword";
        case RewriteStep::Squash: return "squash";
        case RewriteStep::Fixup: return "fixup";
        default: return "drop";
    }
}

static std::string escape_msg(const std::string& m) {
    std::string o;
    for (char c : m) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}
static std::string unescape_msg(std::string_view m) {
    std::string o;
    for (size_t i = 0; i < m.size(); i++) {
        if (m[i] == '\\' && i + 1 < m.size()) { o += m[i + 1] == 'n' ? '\n' : m[i + 1]; i++; }
        else o += m[i];
    }
    return o;
}

void Repository::save_rewrite_state(const RewriteState& st) {
    auto dir = gitdir_ / "tributary-rewrite";
    fs::create_directories(dir);
    std::string s = "label " + escape_msg(st.label) + "\nbranch " + st.branch + "\nonto " + st.onto.hex() +
                    "\norig " + st.orig_head.hex() + "\ncurrent " + st.current.hex() + "\nnext " + std::to_string(st.next) +
                    "\nstopped " + (st.stopped ? "1" : "0") + "\n";
    for (auto& step : st.steps) s += std::string("step ") + op_name(step.op) + " " + step.commit.hex() + " " + escape_msg(step.message) + "\n";
    write_file_atomic(dir / "state", s);
}

void Repository::clear_rewrite_state() {
    std::error_code ec;
    fs::remove_all(gitdir_ / "tributary-rewrite", ec);
}

std::optional<RewriteState> Repository::rewrite_state() const {
    auto d = try_read_file(gitdir_ / "tributary-rewrite" / "state");
    if (!d) return std::nullopt;
    RewriteState st;
    for (auto line : split_lines(*d)) {
        auto sp = line.find(' ');
        auto key = line.substr(0, sp);
        auto val = sp == std::string_view::npos ? std::string_view{} : line.substr(sp + 1);
        if (key == "label") st.label = unescape_msg(val);
        else if (key == "branch") st.branch = std::string(val);
        else if (key == "onto") st.onto = ObjectId::from_hex(val);
        else if (key == "orig") st.orig_head = ObjectId::from_hex(val);
        else if (key == "current") st.current = ObjectId::from_hex(val);
        else if (key == "next") st.next = size_t(std::stoul(std::string(val)));
        else if (key == "stopped") st.stopped = val == "1";
        else if (key == "step") {
            RewriteStep step;
            auto s1 = val.find(' ');
            auto op = val.substr(0, s1);
            auto rest = val.substr(s1 + 1);
            step.op = op == "pick" ? RewriteStep::Pick : op == "reword" ? RewriteStep::Reword : op == "squash" ? RewriteStep::Squash
                      : op == "fixup" ? RewriteStep::Fixup : RewriteStep::Drop;
            step.commit = ObjectId::from_hex(rest.substr(0, 40));
            if (rest.size() > 41) step.message = unescape_msg(rest.substr(41));
            st.steps.push_back(step);
        }
    }
    return st;
}

// Detach HEAD at `id` and make the worktree/index match it, touching only the
// files that differ (keeps untracked files).
static void detach_at(Repository& r, RefDB& refs, const ObjectId& id) {
    auto h = r.head();
    std::optional<ObjectId> from_tree;
    if (h) from_tree = r.odb().read_commit(*h).tree;
    write_file(r.gitdir() / "HEAD", id.hex() + "\n");
    r.switch_tree(from_tree, r.odb().read_commit(id).tree, true);
    (void)refs;
}

ObjectId Repository::commit_from_worktree(const std::vector<ObjectId>& parents, const Signature& author, const std::string& msg) {
    // stage every tracked change (used after resolving conflicts mid-rewrite)
    auto st = status();
    for (auto& f : st.files)
        if (f.conflicted && f.has_markers) throw Error(f.path + " still contains conflict markers.");
    Index idx = load_index();
    for (auto& f : st.files) {
        if (f.change == Change::Untracked && !f.conflicted) continue;
        auto stt = stat_path(wt_ / git_to_path(f.path));
        idx.remove(f.path);
        if (!f.old_path.empty()) idx.remove(f.old_path);
        if (!stt || stt->is_dir) continue;
        uint32_t mode;
        hash_worktree_file(f.path, *stt, mode);
        auto id = odb_->write(ObjType::Blob, worktree_blob(f.path));
        idx.add(make_entry(f.path, id, mode));
    }
    save_index(idx);
    Commit c;
    c.tree = idx.write_tree(*odb_);
    c.parents = parents;
    c.author = author;
    c.committer = signature();
    c.message = msg;
    return write_commit(c);
}

RewriteResult Repository::rewrite_run(RewriteState st) {
    RewriteResult res;
    res.label = st.label;
    for (; st.next < st.steps.size(); st.next++) {
        auto& step = st.steps[st.next];
        if (step.op == RewriteStep::Drop) continue;
        auto c = odb_->read_commit(step.commit);
        if (c.parents.size() > 1) throw Error("Can't rewrite merge commits (" + step.commit.short_hex() + ").");
        bool combine = step.op == RewriteStep::Squash || step.op == RewriteStep::Fixup;
        ObjectId cur_tree = odb_->read_commit(st.current).tree;
        ObjectId parent_tree = c.parents.empty() ? odb_->write(ObjType::Tree, "") : odb_->read_commit(c.parents[0]).tree;
        ObjectId new_tree;
        // fast path: untouched pick on top of its own parent
        if (step.op == RewriteStep::Pick && !c.parents.empty() && c.parents[0] == st.current) {
            st.current = step.commit;
            detach_at(*this, *refs_, st.current);
            continue;
        }
        if (parent_tree == cur_tree) {
            new_tree = c.tree;
        } else {
            Index idx = load_index();
            std::vector<std::string> conflicts;
            three_way(parent_tree, cur_tree, c.tree, "HEAD", step.commit.short_hex() + " (" + c.summary() + ")", idx, conflicts);
            save_index(idx);
            if (!conflicts.empty()) {
                st.stopped = true;
                save_rewrite_state(st);
                res.conflicts = true;
                res.conflicted = conflicts;
                return res;
            }
            new_tree = idx.write_tree(*odb_);
        }
        Commit nc;
        nc.tree = new_tree;
        nc.committer = signature();
        if (combine) {
            auto prev = odb_->read_commit(st.current);
            nc.parents = prev.parents;
            nc.author = prev.author;
            nc.message = !step.message.empty() ? step.message
                         : step.op == RewriteStep::Fixup ? prev.message
                         : trim(prev.message) + "\n\n" + c.message;
        } else {
            nc.parents = {st.current};
            nc.author = c.author;
            nc.message = step.op == RewriteStep::Reword && !step.message.empty() ? step.message : c.message;
            nc.extra_headers.clear();  // drop old signatures
        }
        st.current = write_commit(nc);
        detach_at(*this, *refs_, st.current);
        save_rewrite_state(st);
    }
    // finish: move the branch and re-attach HEAD
    auto who = signature();
    refs_->update(st.branch, st.current, st.label + " (finish): " + shorten_ref(st.branch), who, std::nullopt);
    refs_->set_symbolic("HEAD", st.branch);
    write_file(gitdir_ / "ORIG_HEAD", st.orig_head.hex() + "\n");
    clear_rewrite_state();
    res.new_head = st.current;
    return res;
}

RewriteResult Repository::rewrite(const std::string& label, const std::string& branch, const ObjectId& onto,
                                  std::vector<RewriteStep> steps) {
    if (rewrite_state()) throw Error("Another operation is already in progress. Continue or abort it first.");
    for (auto& f : status().files)
        if (f.change != Change::Untracked) throw Error("Commit or stash your changes before " + to_lower(label) + ".");
    auto tip = refs_->resolve(branch);
    if (!tip) throw Error("no branch " + branch);
    RewriteState st;
    st.label = label;
    st.branch = branch;
    st.onto = onto;
    st.orig_head = *tip;
    st.current = onto;
    st.steps = std::move(steps);
    save_rewrite_state(st);
    try {
        detach_at(*this, *refs_, onto);
        return rewrite_run(st);
    } catch (...) {
        if (auto cur = rewrite_state(); cur && !cur->stopped) {
            // unexpected failure: restore the original branch
            try { rewrite_abort(); } catch (...) {}
        }
        throw;
    }
}

RewriteResult Repository::rewrite_continue() {
    auto st = rewrite_state();
    if (!st) throw Error("Nothing to continue.");
    auto& step = st->steps[st->next];
    auto c = odb_->read_commit(step.commit);
    bool combine = step.op == RewriteStep::Squash || step.op == RewriteStep::Fixup;
    std::vector<ObjectId> parents;
    Signature author = c.author;
    std::string msg = step.op == RewriteStep::Reword && !step.message.empty() ? step.message : c.message;
    if (combine) {
        auto prev = odb_->read_commit(st->current);
        parents = prev.parents;
        author = prev.author;
        msg = !step.message.empty() ? step.message : step.op == RewriteStep::Fixup ? prev.message : trim(prev.message) + "\n\n" + c.message;
    } else {
        parents = {st->current};
    }
    st->current = commit_from_worktree(parents, author, msg);
    detach_at(*this, *refs_, st->current);
    st->next++;
    st->stopped = false;
    save_rewrite_state(*st);
    return rewrite_run(*st);
}

void Repository::rewrite_abort() {
    auto st = rewrite_state();
    if (!st) return;
    // restore the branch exactly as it was
    refs_->set_symbolic("HEAD", st->branch);
    reset_hard(st->orig_head);
    clear_rewrite_state();
}

RewriteResult Repository::rebase_onto(const std::string& upstream_ref) {
    auto h = head();
    auto up = refs_->resolve(upstream_ref);
    if (!h || !up) throw Error("nothing to rebase");
    auto branch = refs_->head_target();
    if (branch.empty()) throw Error("You're not on a branch.");
    auto upc = odb_->peel_to_commit(*up);
    if (is_ancestor(upc, *h)) return {false, {}, *h, "Rebase"};
    auto base = merge_base(*h, upc);
    if (!base) throw Error("These branches don't share any history.");
    auto mine = commits_between(*base, *h);
    std::reverse(mine.begin(), mine.end());
    std::vector<RewriteStep> steps;
    for (auto& id : mine) {
        if (odb_->read_commit(id).parents.size() > 1) continue;  // merges are flattened away, like git rebase
        steps.push_back({RewriteStep::Pick, id, ""});
    }
    return rewrite("Rebase", branch, upc, steps);
}

// Builds steps for all commits from `oldest`'s parent up to HEAD.
static std::vector<ObjectId> chain_to_head(Repository& r, const ObjectId& oldest, ObjectId& onto) {
    auto c = r.odb().read_commit(oldest);
    if (c.parents.empty()) throw Error("Can't rewrite the root commit yet.");
    onto = c.parents[0];
    auto chain = r.commits_between(onto, *r.head());
    std::reverse(chain.begin(), chain.end());
    return chain;
}

RewriteResult Repository::reword(const ObjectId& commit, const std::string& message) {
    auto branch = refs_->head_target();
    if (branch.empty()) throw Error("You're not on a branch.");
    ObjectId onto;
    auto chain = chain_to_head(*this, commit, onto);
    std::vector<RewriteStep> steps;
    for (auto& id : chain) steps.push_back({id == commit ? RewriteStep::Reword : RewriteStep::Pick, id, id == commit ? message : ""});
    return rewrite("Reword", branch, onto, steps);
}

RewriteResult Repository::squash_commits(const std::vector<ObjectId>& commits, const ObjectId& into, const std::string& message) {
    auto branch = refs_->head_target();
    if (branch.empty()) throw Error("You're not on a branch.");
    // find the oldest involved commit
    auto h = *head();
    auto order = commits_between(ObjectId{}, h);  // newest first, whole branch
    std::map<ObjectId, size_t> pos;
    for (size_t i = 0; i < order.size(); i++) pos[order[i]] = i;
    ObjectId oldest = into;
    for (auto& c : commits) if (pos.count(c) && pos[c] > pos[oldest]) oldest = c;
    ObjectId onto;
    auto chain = chain_to_head(*this, oldest, onto);
    std::set<ObjectId> moving(commits.begin(), commits.end());
    std::vector<RewriteStep> steps;
    for (auto& id : chain) {
        if (moving.count(id) && id != into) continue;
        steps.push_back({RewriteStep::Pick, id, ""});
        if (id == into) {
            // squash the others (oldest first) right after the target
            for (auto& m : chain)
                if (moving.count(m) && m != into) steps.push_back({RewriteStep::Squash, m, ""});
            steps.back().message = message;
        }
    }
    return rewrite("Squash", branch, onto, steps);
}

RewriteResult Repository::reorder(const std::vector<ObjectId>& commits, const ObjectId& before) {
    auto branch = refs_->head_target();
    if (branch.empty()) throw Error("You're not on a branch.");
    auto h = *head();
    auto order = commits_between(ObjectId{}, h);
    std::map<ObjectId, size_t> pos;
    for (size_t i = 0; i < order.size(); i++) pos[order[i]] = i;
    ObjectId oldest = commits.front();
    for (auto& c : commits) if (pos[c] > pos[oldest]) oldest = c;
    if (!before.is_zero() && pos[before] > pos[oldest]) oldest = before;
    ObjectId onto;
    auto chain = chain_to_head(*this, oldest, onto);  // oldest first
    std::set<ObjectId> moving(commits.begin(), commits.end());
    std::vector<ObjectId> moving_ordered;
    for (auto& id : chain) if (moving.count(id)) moving_ordered.push_back(id);
    std::vector<RewriteStep> steps;
    for (auto& id : chain) {
        if (moving.count(id)) continue;
        if (id == before) for (auto& m : moving_ordered) steps.push_back({RewriteStep::Pick, m, ""});
        steps.push_back({RewriteStep::Pick, id, ""});
    }
    if (before.is_zero()) for (auto& m : moving_ordered) steps.push_back({RewriteStep::Pick, m, ""});
    return rewrite("Reorder", branch, onto, steps);
}

RewriteResult Repository::cherry_pick_onto(const std::vector<ObjectId>& commits, const std::string& target_branch) {
    std::string target = starts_with(target_branch, "refs/") ? target_branch : "refs/heads/" + target_branch;
    auto tip = refs_->resolve(target);
    if (!tip) throw Error("no branch " + target_branch);
    // switch to the target branch first (Desktop does the same)
    if (refs_->head_target() != target) checkout_branch(shorten_ref(target));
    // oldest first
    auto all = log(*head(), 1);
    (void)all;
    std::vector<ObjectId> ordered = commits;
    std::sort(ordered.begin(), ordered.end(), [&](const ObjectId& a, const ObjectId& b) {
        return odb_->read_commit(a).committer.time < odb_->read_commit(b).committer.time;
    });
    std::vector<RewriteStep> steps;
    for (auto& c : ordered) steps.push_back({RewriteStep::Pick, c, ""});
    return rewrite("Cherry-pick", target, *tip, steps);
}

// ================================================================ utils
std::string format_relative_time(int64_t t, int64_t now) {
    int64_t d = now - t;
    if (d < 0) d = 0;
    auto plural = [](int64_t n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (d < 60) return "just now";
    if (d < 3600) return plural(d / 60, "minute");
    if (d < 86400) return plural(d / 3600, "hour");
    if (d < 86400 * 30) return plural(d / 86400, "day");
    if (d < 86400 * 365) return plural(d / (86400 * 30), "month");
    return plural(d / (86400 * 365), "year");
}

bool is_valid_branch_name(const std::string& n) {
    return !n.empty() && n != "HEAD" && valid_ref_name("refs/heads/" + n);
}

std::string sanitize_branch_name(const std::string& n) {
    std::string out;
    for (char c : n) {
        unsigned char u = (unsigned char)c;
        if (u <= ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\')
            out += '-';
        else out += c;
    }
    while (out.find("..") != std::string::npos) out.replace(out.find(".."), 2, ".");
    while (out.find("--") != std::string::npos) out.replace(out.find("--"), 2, "-");
    while (!out.empty() && (out.back() == '.' || out.back() == '/' || out.back() == '-')) out.pop_back();
    while (!out.empty() && (out.front() == '-' || out.front() == '/' || out.front() == '.')) out.erase(out.begin());
    if (out.size() >= 5 && out.substr(out.size() - 5) == ".lock") out.resize(out.size() - 5);
    return out;
}

}  // namespace git
