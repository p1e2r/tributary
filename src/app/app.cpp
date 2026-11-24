// SPDX-License-Identifier: MIT
#include "app.hpp"
#include "../git/lfs.hpp"

#include <tuple>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <tuple>

#include "../net/http.hpp"
#include "../net/json.hpp"

namespace app {
using gfx::Color;
using gfx::FontId;
using gfx::Align;

namespace fs = std::filesystem;

// ================================================================ Jobs
Jobs::Jobs(plat::Window& w) : win_(w) {
    th_ = std::thread([this] {
        for (;;) {
            std::pair<std::string, std::function<void()>> job;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
                if (quit_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
                label_ = job.first;
                progress_text_.clear();
                progress_ = -1;
                busy_ = true;
            }
            win_.wake();
            try {
                job.second();
            } catch (const std::exception& e) {
                std::string msg = e.what();
                std::string label = job.first;
                post([msg, label, this] { (void)this; });
            }
            {
                std::lock_guard lk(mu_);
                busy_ = !q_.empty();
                label_.clear();
            }
            win_.wake();
        }
    });
}

Jobs::~Jobs() {
    {
        std::lock_guard lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Jobs::run(std::string label, std::function<void()> fn) {
    {
        std::lock_guard lk(mu_);
        q_.push_back({std::move(label), std::move(fn)});
        busy_ = true;
    }
    cv_.notify_one();
}

void Jobs::post(std::function<void()> fn) {
    {
        std::lock_guard lk(mu_);
        results_.push_back(std::move(fn));
    }
    win_.wake();
}

void Jobs::drain() {
    std::vector<std::function<void()>> r;
    {
        std::lock_guard lk(mu_);
        r.swap(results_);
    }
    for (auto& f : r) f();
}

std::string Jobs::current_label() {
    std::lock_guard lk(mu_);
    return label_;
}

void Jobs::set_progress(const std::string& text, float fraction) {
    {
        std::lock_guard lk(mu_);
        progress_text_ = text;
        progress_ = fraction;
    }
    win_.wake();
}

std::pair<std::string, float> Jobs::progress() {
    std::lock_guard lk(mu_);
    return {progress_text_, progress_};
}

// ================================================================ App
App::App(plat::Window& w, gfx::Renderer& rr) : ui(rr, w), jobs(w), win(w), r(rr) {
    state_file = plat::app_data_dir() + "/state.json";
    settings.clone_dir = plat::default_clone_dir();
    load_state();
    load_git_identity();
    apply_theme();
    r.set_zoom(settings.zoom);
    if (!repos.empty()) open_repo(std::clamp(current, 0, int(repos.size()) - 1));
    if (account.valid()) load_gh_repos();
}

App::~App() {
    alive_->store(false);
    save_state();
}

void App::load_git_identity() {
    try {
        auto cfg = git::Config::load(git::global_config_path());
        git_user_name = cfg.get("user.name").value_or("");
        git_user_email = cfg.get("user.email").value_or("");
    } catch (...) {}
}

void App::apply_theme() {
    bool dark = settings.theme == "dark" || (settings.theme == "system" && win.is_dark_mode());
    ui.theme = dark ? ui::Theme::dark_theme() : ui::Theme::light();
}

void App::toast(const std::string& text, bool error) {
    toasts.push_back({text, error, ui.time + (error ? 7.0 : 4.0)});
}

void App::show_error(const std::string& title, const std::string& text) {
    error_title = title;
    error_text = text;
    dialog = Dialog::Error;
}

std::string App::repo_display_path(const std::string& p) const {
    auto home = git::path_to_git(git::home_dir());
    if (git::starts_with(p, home)) return "~" + p.substr(home.size());
    return p;
}

std::string App::remote_name() const {
    if (!rs) return "origin";
    if (!rs->upstream.empty()) {
        auto rest = rs->upstream.substr(13);
        return rest.substr(0, rest.find('/'));
    }
    for (auto& r2 : rs->remotes) if (r2.name == "origin") return "origin";
    return rs->remotes.empty() ? "origin" : rs->remotes[0].name;
}

std::string App::github_url() const {
    if (!rs || !rs->github) return "";
    return account.html_base() + "/" + rs->github->first + "/" + rs->github->second;
}

// ================================================================ persistence
void App::load_state() {
    auto data = git::try_read_file(state_file);
    if (!data) return;
    try {
        auto v = json::parse(*data);
        for (auto& r2 : v["repositories"].arr()) {
            RepoEntry e;
            e.path = r2["path"].str();
            e.name = r2["name"].str(git::path_to_git(fs::path(git::git_to_path(e.path)).filename()));
            e.owner = r2["owner"].str();
            e.pinned = r2["pinned"].boolean(false);
            if (!e.path.empty()) repos.push_back(e);
        }
        current = int(v["selected"].integer(0));
        auto& s = v["settings"];
        settings.theme = s["theme"].str(settings.theme);
        settings.editor_cmd = s["editor"].str();
        settings.shell_cmd = s["shell"].str();
        settings.clone_dir = s["clone_dir"].str(settings.clone_dir);
        settings.oauth_client_id = s["oauth_client_id"].str();
        settings.default_branch = s["default_branch"].str("main");
        settings.confirm_discard = s["confirm_discard"].boolean(true);
        settings.confirm_force_push = s["confirm_force_push"].boolean(true);
        settings.confirm_remove_repo = s["confirm_remove_repo"].boolean(true);
        settings.sidebar_width = float(s["sidebar_width"].num(250));
        settings.split_diff = s["split_diff"].boolean(false);
        settings.hide_whitespace = s["hide_whitespace"].boolean(false);
        settings.zoom = float(s["zoom"].num(1.0));
        settings.image_mode = int(s["image_mode"].integer(0));
        auto& a = v["account"];
        account.login = a["login"].str();
        account.name = a["name"].str();
        account.email = a["email"].str();
        account.endpoint = a["endpoint"].str("https://api.github.com");
        if (!account.login.empty()) account.token = plat::secret_get("github:" + account.login);
        for (auto& e : a["emails"].arr()) account.emails.push_back(e.str());
        auto& g = v["enterprise"];
        enterprise.login = g["login"].str();
        enterprise.name = g["name"].str();
        enterprise.email = g["email"].str();
        enterprise.endpoint = g["endpoint"].str("https://api.github.com");
        for (auto& e : g["emails"].arr()) enterprise.emails.push_back(e.str());
        if (!enterprise.login.empty()) enterprise.token = plat::secret_get("ghe:" + enterprise.host() + ":" + enterprise.login);
    } catch (...) {
    }
}

void App::save_state() {
    json::Value v;
    json::Array arr;
    for (auto& e : repos) {
        json::Value o;
        o.set("path", e.path);
        o.set("name", e.name);
        o.set("owner", e.owner);
        o.set("pinned", e.pinned);
        arr.push_back(o);
    }
    v.set("repositories", json::Value(arr));
    v.set("selected", current);
    json::Value s;
    s.set("theme", settings.theme);
    s.set("editor", settings.editor_cmd);
    s.set("shell", settings.shell_cmd);
    s.set("clone_dir", settings.clone_dir);
    s.set("oauth_client_id", settings.oauth_client_id);
    s.set("default_branch", settings.default_branch);
    s.set("confirm_discard", settings.confirm_discard);
    s.set("confirm_force_push", settings.confirm_force_push);
    s.set("confirm_remove_repo", settings.confirm_remove_repo);
    s.set("sidebar_width", double(settings.sidebar_width));
    s.set("split_diff", settings.split_diff);
    s.set("hide_whitespace", settings.hide_whitespace);
    s.set("zoom", double(settings.zoom));
    s.set("image_mode", settings.image_mode);
    v.set("settings", s);
    json::Value a;
    a.set("login", account.login);
    a.set("name", account.name);
    a.set("email", account.email);
    a.set("endpoint", account.endpoint);
    {
        json::Array em;
        for (auto& e : account.emails) em.push_back(e);
        a.set("emails", json::Value(em));
    }
    v.set("account", a);
    json::Value g;
    g.set("login", enterprise.login);
    g.set("name", enterprise.name);
    g.set("email", enterprise.email);
    g.set("endpoint", enterprise.endpoint);
    {
        json::Array em;
        for (auto& e : enterprise.emails) em.push_back(e);
        g.set("emails", json::Value(em));
    }
    v.set("enterprise", g);
    try {
        git::write_file_atomic(git::git_to_path(state_file), v.dump(true));
    } catch (...) {
    }
}

const gh::Account* App::account_for_url(const std::string& url) const {
    std::string host;
    try {
        if (url.find("://") != std::string::npos) host = net::Url::parse(url).host;
        else if (auto at = url.find('@'); at != std::string::npos) host = url.substr(at + 1, url.find(':', at) - at - 1);
    } catch (...) {}
    if (enterprise.valid() && host == enterprise.host()) return &enterprise;
    if (account.valid() && (host == "github.com" || host.empty())) return &account;
    return nullptr;
}

// ================================================================ credentials
git::CredentialProvider App::credentials() {
    gh::Account acc = account, ghe = enterprise;
    return [this, acc, ghe](const std::string& url, int attempt) -> std::optional<git::Credentials> {
        auto host = net::Url::parse(url).host;
        if (acc.valid() && attempt == 0 && host == "github.com") return git::Credentials{acc.login, acc.token};
        if (ghe.valid() && attempt == 0 && host == ghe.host()) return git::Credentials{ghe.login, ghe.token};
        // ask the user on the UI thread and wait
        std::promise<std::optional<git::Credentials>> prom;
        auto fut = prom.get_future();
        jobs.post([this, url, &prom] {
            cred_d.url = url;
            cred_d.user.clear();
            cred_d.pass.clear();
            cred_d.promise = &prom;
            dialog = Dialog::Credentials;
        });
        return fut.get();
    };
}

// ================================================================ repos
void App::add_repo_path(const std::string& path_in, bool select) {
    auto wt = git::Repository::discover(git::git_to_path(path_in));
    if (!wt) {
        show_error("Not a Git repository", repo_display_path(path_in) +
                   " doesn't look like a Git repository.\n\nYou can create a new repository there instead from File > New repository.");
        return;
    }
    std::string path = git::path_to_git(fs::canonical(*wt));
    for (int i = 0; i < int(repos.size()); i++)
        if (repos[size_t(i)].path == path) { if (select) open_repo(i); return; }
    RepoEntry e;
    e.path = path;
    e.name = git::path_to_git(fs::path(git::git_to_path(path)).filename());
    try {
        auto repo = git::Repository::open(git::git_to_path(path));
        for (auto& rem : repo.remotes())
            if (auto gh = gh::parse_github_remote(rem.url)) { e.owner = gh->first; break; }
    } catch (...) {}
    repos.push_back(e);
    std::sort(repos.begin(), repos.end(), [](auto& a, auto& b) { return git::to_lower(a.name) < git::to_lower(b.name); });
    save_state();
    if (select)
        for (int i = 0; i < int(repos.size()); i++)
            if (repos[size_t(i)].path == path) open_repo(i);
}

void App::toggle_pin(int index) {
    if (index < 0 || index >= int(repos.size())) return;
    auto& e = repos[size_t(index)];
    e.pinned = !e.pinned;
    toast(e.pinned ? "Pinned " + e.name : "Unpinned " + e.name);
    save_state();
}

void App::remove_repo(int index) {
    if (index < 0 || index >= int(repos.size())) return;
    repos.erase(repos.begin() + index);
    if (current == index) {
        rs.reset();
        current = -1;
        if (!repos.empty()) open_repo(0);
    } else if (current > index) current--;
    save_state();
}

void App::open_repo(int index) {
    if (index < 0 || index >= int(repos.size())) return;
    current = index;
    rs = std::make_unique<RepoState>();
    rs->path = repos[size_t(index)].path;
    rs->name = repos[size_t(index)].name;
    tab = 0;
    win.set_title(rs->name + " — Tributary");
    refresh(true);
    load_history(false);
    save_state();
}

// Gather everything the views need into a snapshot (worker thread).
struct Snapshot {
    git::WorkingStatus status;
    std::vector<git::BranchInfo> branches;
    std::string branch, upstream, default_branch, head_summary;
    std::optional<ObjectId> head;
    std::vector<git::Remote> remotes;
    std::vector<git::StashInfo> stashes;
    std::vector<std::pair<std::string, ObjectId>> tags;
    int ahead = 0, behind = 0;
    int64_t last_fetched = 0;
    bool fork_contrib = false;
};

static Snapshot take_snapshot(const std::string& path, bool full) {
    Snapshot s;
    auto repo = git::Repository::open(git::git_to_path(path));
    s.status = repo.status();
    s.branch = repo.current_branch();
    s.head = repo.head();
    if (s.head) {
        try { s.head_summary = repo.odb().read_commit(*s.head).summary(); } catch (...) {}
    }
    s.remotes = repo.remotes();
    s.upstream = s.branch.empty() ? "" : repo.upstream_of(s.branch);
    if (!s.upstream.empty() && s.head)
        if (auto u = repo.refs().resolve(s.upstream)) std::tie(s.ahead, s.behind) = repo.ahead_behind(*s.head, *u);
    for (auto& rem : s.remotes) {
        auto t = repo.refs().read_raw("refs/remotes/" + rem.name + "/HEAD");
        if (t && git::starts_with(*t, "ref:")) {
            auto target = git::trim(t->substr(4));
            s.default_branch = target.substr(13 + rem.name.size() + 1);
            break;
        }
    }
    if (full) {
        s.branches = repo.branches(true);
        s.stashes = repo.stashes();
        s.tags = repo.tags();
    }
    if (auto st = git::stat_path(repo.gitdir() / "FETCH_HEAD")) s.last_fetched = st->mtime_s;
    s.fork_contrib = repo.config().repo.get("tributary.forkcontributiontarget").value_or("") == "parent";
    return s;
}

void App::refresh(bool full) {
    if (!rs) return;
    auto path = rs->path;
    rs->loading = true;
    last_status_refresh = ui.time;
    jobs.run("Refreshing", [this, path, full] {
        try {
            auto snap = std::make_shared<Snapshot>(take_snapshot(path, full));
            jobs.post([this, path, snap, full] {
                if (!rs || rs->path != path) return;
                rs->loading = false;
                rs->loaded = true;
                rs->error.clear();
                rs->status = std::move(snap->status);
                rs->branch = snap->branch;
                bool head_changed = rs->head != snap->head;
                rs->head = snap->head;
                rs->head_summary = snap->head_summary;
                rs->remotes = snap->remotes;
                rs->upstream = snap->upstream;
                rs->ahead = snap->ahead;
                rs->behind = snap->behind;
                rs->default_branch = snap->default_branch;
                rs->last_fetched = snap->last_fetched;
                if (full) {
                    rs->branches = std::move(snap->branches);
                    rs->stashes = std::move(snap->stashes);
                    rs->tags = std::move(snap->tags);
                }
                rs->github.reset();
                rs->fork_contrib = snap->fork_contrib;
                // PRs and the "default branch" of a fork you contribute to come from its parent
                std::string gh_remote = rs->fork_contrib ? "upstream" : remote_name();
                for (auto& rem : rs->remotes)
                    if (rem.name == gh_remote) {
                        if (auto* acc = account_for_url(rem.url)) rs->github = gh::parse_github_remote(rem.url, acc->host());
                        if (!rs->github) rs->github = gh::parse_github_remote(rem.url);
                    }
                load_checks();
                if (current >= 0 && rs->github) repos[size_t(current)].owner = rs->github->first;
                // keep selections only for files that still exist
                std::map<std::string, FileSel> sel;
                for (auto& f : rs->status.files) {
                    auto it = rs->sel.find(f.path);
                    sel[f.path] = it != rs->sel.end() ? it->second : FileSel{};
                }
                rs->sel = std::move(sel);
                bool still = false;
                for (auto& f : rs->status.files) if (f.path == rs->selected_file) still = true;
                if (!still) rs->selected_file = rs->status.files.empty() ? "" : rs->status.files[0].path;
                if (!rs->selected_file.empty()) select_file(rs->selected_file);
                else rs->diff = {};
                if (head_changed) load_history(false);
            });
        } catch (const std::exception& e) {
            std::string msg = e.what();
            jobs.post([this, path, msg] {
                if (!rs || rs->path != path) return;
                rs->loading = false;
                rs->error = msg;
                if (current >= 0) repos[size_t(current)].missing = !fs::exists(git::git_to_path(path));
            });
        }
    });
}

void App::load_history(bool more) {
    if (!rs || rs->history_loading) return;
    if (more && rs->history_complete) return;
    auto path = rs->path;
    size_t skip = more ? rs->history.size() : 0;
    rs->history_loading = true;
    std::string filter = rs->history_filter;
    jobs.run("Loading history", [this, path, skip, more] {
        std::vector<git::CommitInfo> list;
        bool complete = false;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            if (auto h = repo.head()) {
                list = repo.log(*h, 200, skip);
                complete = list.size() < 200;
            } else complete = true;
        } catch (...) {}
        auto shared = std::make_shared<std::vector<git::CommitInfo>>(std::move(list));
        jobs.post([this, path, shared, complete, more] {
            if (!rs || rs->path != path) return;
            rs->history_loading = false;
            if (!more) rs->history.clear();
            for (auto& c : *shared) rs->history.push_back(std::move(c));
            rs->history_complete = complete;
            if (!rs->selected_commit && !rs->history.empty() && tab == 1) select_commit(rs->history[0].id);
        });
    });
}

static bool is_image_path(const std::string& p) {
    auto e = git::to_lower(fs::path(git::git_to_path(p)).extension().string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".gif" || e == ".webp" || e == ".ico" || e == ".bmp";
}

// Decode both sides of an image change for the image diff views (worker thread).
static void load_images(DiffView& dv, const std::string& path, const git::Bytes& old, const git::Bytes& neu) {
    bool img = is_image_path(path) || gfx::is_image_data(old) || gfx::is_image_data(neu);
    if (!img || !dv.diff.binary) return;
    dv.image = true;
    dv.old_bytes = old.size();
    dv.new_bytes = neu.size();
    try {
        if (!old.empty()) dv.img_old = std::make_shared<gfx::Image>(gfx::decode_image(old));
        if (!neu.empty()) dv.img_new = std::make_shared<gfx::Image>(gfx::decode_image(neu));
        if (dv.img_old && dv.img_new) dv.img_diff = std::make_shared<gfx::Image>(gfx::difference_image(*dv.img_old, *dv.img_new));
    } catch (const std::exception& e) {
        dv.img_error = e.what();
    }
}

void App::select_file(const std::string& path) {
    if (!rs) return;
    rs->selected_file = path;
    const git::StatusEntry* se = nullptr;
    for (auto& f : rs->status.files) if (f.path == path) se = &f;
    if (!se) return;
    git::StatusEntry entry = *se;
    auto repo_path = rs->path;
    uint64_t token = ++diff_token;
    bool keep = rs->diff.path == path;
    rs->diff.path = path;
    rs->diff.loading = true;
    rs->diff.token = token;
    if (!keep) { rs->diff.diff = {}; rs->diff.full.clear(); }
    int ctx = diff_context.count(path) ? diff_context[path] : 3;
    bool ws = settings.hide_whitespace;
    jobs.run("Loading diff", [this, repo_path, entry, token, ctx, ws] {
        auto dv = std::make_shared<DiffView>();
        dv->path = entry.path;
        dv->token = token;
        try {
            auto repo = git::Repository::open(git::git_to_path(repo_path));
            if (entry.path.back() == '/') {
                dv->error = "This is a nested Git repository or submodule.";
            } else {
                git::Bytes old = (entry.change == git::Change::Untracked || entry.change == git::Change::Added)
                                     ? ""
                                     : repo.head_content(entry.old_path.empty() ? entry.path : entry.old_path);
                git::Bytes neu = entry.change == git::Change::Deleted ? "" : repo.worktree_content(entry.path);
                dv->diff = git::diff_text(old, neu, ctx, ws);
                dv->ignore_ws = ws;
                if (!dv->diff.binary && !dv->diff.too_large) dv->full = git::full_diff_lines(old, neu);
                load_images(*dv, entry.path, old, neu);
            }
        } catch (const std::exception& e) {
            dv->error = e.what();
        }
        jobs.post([this, repo_path, dv] {
            if (!rs || rs->path != repo_path || rs->diff.token != dv->token) return;
            auto sel = rs->sel.find(dv->path);
            // drop a stale partial selection if the diff changed shape
            if (sel != rs->sel.end() && sel->second.lines && sel->second.lines->size() != dv->full.size())
                sel->second.lines.reset();
            rs->diff = std::move(*dv);
        });
    });
}

void App::select_commit(const ObjectId& id) {
    if (!rs) return;
    rs->selected_commit = id;
    rs->commit_files.clear();
    rs->commit_diff = {};
    rs->selected_commit_file.clear();
    auto path = rs->path;
    jobs.run("Loading commit", [this, path, id] {
        std::vector<git::ChangedFile> files;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            files = repo.commit_changes(id);
        } catch (...) {}
        auto shared = std::make_shared<std::vector<git::ChangedFile>>(std::move(files));
        jobs.post([this, path, id, shared] {
            if (!rs || rs->path != path || rs->selected_commit != id) return;
            rs->commit_files = std::move(*shared);
            if (!rs->commit_files.empty()) select_commit_file(rs->commit_files[0].path);
        });
    });
}

void App::select_commit_file(const std::string& file) {
    if (!rs) return;
    rs->selected_commit_file = file;
    git::ChangedFile cf;
    bool found = false;
    for (auto& f : rs->commit_files) if (f.path == file) { cf = f; found = true; }
    if (!found) return;
    auto path = rs->path;
    uint64_t token = ++diff_token;
    rs->commit_diff.token = token;
    rs->commit_diff.loading = true;
    rs->commit_diff.path = file;
    int ctx = diff_context.count(file) ? diff_context[file] : 3;
    bool ws = settings.hide_whitespace;
    jobs.run("Loading diff", [this, path, cf, token, ctx, ws] {
        auto dv = std::make_shared<DiffView>();
        dv->path = cf.path;
        dv->token = token;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            dv->diff = repo.commit_file_diff(cf, ctx, ws);
            dv->ignore_ws = ws;
            if (dv->diff.binary)
                load_images(*dv, cf.path, cf.old_id.is_zero() ? "" : repo.blob(cf.old_id), cf.new_id.is_zero() ? "" : repo.blob(cf.new_id));
        } catch (const std::exception& e) { dv->error = e.what(); }
        jobs.post([this, path, dv] {
            if (!rs || rs->path != path || rs->commit_diff.token != dv->token) return;
            rs->commit_diff = std::move(*dv);
        });
    });
}

void App::load_stash_view() {
    if (!rs) return;
    auto path = rs->path;
    rs->viewing_stash = true;
    rs->stash_files.clear();
    rs->stash_diff = {};
    jobs.run("Loading stash", [this, path] {
        std::vector<git::ChangedFile> files;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            files = repo.stash_changes(0);
        } catch (...) {}
        auto shared = std::make_shared<std::vector<git::ChangedFile>>(std::move(files));
        jobs.post([this, path, shared] {
            if (!rs || rs->path != path) return;
            rs->stash_files = std::move(*shared);
            if (!rs->stash_files.empty()) {
                rs->selected_stash_file = rs->stash_files[0].path;
                auto cf = rs->stash_files[0];
                auto repo = git::Repository::open(git::git_to_path(path));
                rs->stash_diff.diff = repo.commit_file_diff(cf);
                rs->stash_diff.path = cf.path;
            }
        });
    });
}

// ================================================================ operations
// Runs a repository operation on the worker, reports errors, then refreshes.
template <typename F>
static void repo_op(App& a, const std::string& label, F fn, std::function<void()> after = {}) {
    if (!a.rs) return;
    auto path = a.rs->path;
    auto creds = a.credentials();
    a.jobs.run(label, [&a, path, label, fn, after, creds] {
        std::string err;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            fn(repo);
            // checkouts and merges that wrote Git LFS pointers: fetch the real files
            if (!repo.lfs_missing().empty()) {
                auto br = repo.current_branch();
                auto remote = repo.config().get("branch." + br + ".remote").value_or("origin");
                if (repo.config().get("remote." + remote + ".url")) git::lfs::pull(repo, remote, creds, {});
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        a.jobs.post([&a, path, err, label, after] {
            if (!err.empty()) a.show_error(label + " failed", err);
            else if (after) after();
            if (a.rs && a.rs->path == path) {
                a.refresh(true);
                a.load_history(false);
            }
        });
    });
}

void App::commit() {
    if (!rs) return;
    std::vector<git::CommitSelection> files;
    for (auto& f : rs->status.files) {
        auto it = rs->sel.find(f.path);
        if (it == rs->sel.end() || !it->second.included) continue;
        git::CommitSelection s;
        s.path = f.path;
        if (it->second.lines) {
            bool any = false, all = true;
            for (bool b : *it->second.lines) { any |= b; all &= b; }
            if (!any) continue;
            if (!all) s.lines = it->second.lines;
        }
        files.push_back(s);
    }
    if (files.empty() && !rs->amending && !rs->status.merging && !rs->status.rewrite) { toast("Select at least one file to commit.", true); return; }
    // GitHub rejects files over 100 MB: warn first (once per commit attempt)
    if (dialog != Dialog::LargeFiles) {
        large_d.files.clear();
        for (auto& f : files) {
            std::error_code ec;
            auto sz = fs::file_size(git::git_to_path(rs->path) / git::git_to_path(f.path), ec);
            if (!ec && sz > 100ull * 1024 * 1024) large_d.files.push_back({f.path, sz});
        }
        if (!large_d.files.empty()) { dialog = Dialog::LargeFiles; return; }
    }
    std::string summary = rs->summary, desc = rs->description;
    bool amend = rs->amending;
    // co-authors: "@login" (resolved via the API) or "Name <email>"
    std::vector<std::string> co_raw;
    if (rs->show_coauthors) {
        std::string cur;
        for (char ch : rs->coauthors + ",") {
            if (ch == ',' || ch == ';') { if (!git::trim(cur).empty()) co_raw.push_back(git::trim(cur)); cur.clear(); }
            else cur += ch;
        }
    }
    auto acc = account_for_url(rs->remotes.empty() ? "" : rs->remotes[0].url);
    gh::Account co_acc = acc ? *acc : account;
    repo_op(*this, "Commit", [files, summary, desc, amend, co_raw, co_acc](git::Repository& repo) {
        std::vector<std::pair<std::string, std::string>> co;
        for (auto& c : co_raw) {
            auto lt = c.find('<'), gt = c.find('>');
            if (lt != std::string::npos && gt != std::string::npos) co.push_back({git::trim(c.substr(0, lt)), c.substr(lt + 1, gt - lt - 1)});
            else if (!c.empty() && c[0] == '@' && co_acc.valid()) co.push_back(gh::coauthor_for_login(co_acc, c.substr(1)));
            else throw git::Error("Couldn't understand co-author \"" + c + "\". Use @username or Name <email>.");
        }
        repo.commit(files, summary, desc, amend, co);
    }, [this, summary] {
        if (!rs) return;
        rs->last_commit_summary = summary;
        rs->last_commit_for_undo = ObjectId{};
        rs->summary.clear();
        rs->description.clear();
        rs->amending = false;
        rs->sel.clear();
        rs->coauthors.clear();
        rs->show_coauthors = false;
    });
}

void App::undo_commit() {
    if (!rs) return;
    // restore the message so it can be edited again
    std::string summary, desc;
    if (!rs->history.empty()) {
        summary = rs->history[0].commit.summary();
        desc = rs->history[0].commit.body();
    }
    repo_op(*this, "Undo commit", [](git::Repository& repo) { repo.undo_last_commit(); },
            [this, summary, desc] {
                if (!rs) return;
                rs->summary = summary;
                rs->description = desc;
                rs->last_commit_for_undo.reset();
            });
}

static void with_progress(App& a, git::ProgressFn& out) {
    out = [&a](const git::Progress& p) {
        std::string text = p.phase;
        if (!p.remote_message.empty()) text = p.remote_message;
        else if (p.total) text += " " + std::to_string(p.done) + "/" + std::to_string(p.total);
        else if (p.done && p.phase == "Receiving objects") text += " " + std::to_string(p.done / 1024) + " KiB";
        a.jobs.set_progress(text, p.total ? float(p.done) / float(p.total) : -1.f);
    };
}

void App::fetch() {
    if (!rs || rs->remotes.empty()) return;
    auto remote = remote_name();
    auto creds = credentials();
    git::ProgressFn prog;
    with_progress(*this, prog);
    repo_op(*this, "Fetch", [remote, creds, prog](git::Repository& repo) { git::fetch(repo, remote, creds, prog); });
}

void App::pull() {
    if (!rs) return;
    auto creds = credentials();
    git::ProgressFn prog;
    with_progress(*this, prog);
    auto result = std::make_shared<git::MergeOutcome>();
    repo_op(*this, "Pull", [creds, prog, result](git::Repository& repo) { *result = git::pull(repo, creds, prog); },
            [this, result] {
                if (result->kind == git::MergeOutcome::Conflicts) dialog = Dialog::Conflicts;
            });
}

void App::push(bool force) {
    if (!rs) return;
    auto remote = remote_name();
    auto branch = rs->branch;
    if (branch.empty()) { show_error("Can't push", "You're not on a branch (detached HEAD)."); return; }
    bool set_up = rs->upstream.empty();
    auto creds = credentials();
    git::ProgressFn prog;
    with_progress(*this, prog);
    repo_op(*this, set_up ? "Publish branch" : "Push", [remote, branch, set_up, creds, prog, force](git::Repository& repo) {
        git::push(repo, remote, branch, creds, prog, set_up, force);
        try { git::push_tags(repo, remote, creds); } catch (...) {}  // like GitHub Desktop: tags go out with the next push
    });
}

void App::publish_branch() { push(false); }

void App::checkout(const std::string& branch) {
    if (has_changes() && !rs->status.merging) {
        switch_d = {branch, 0};
        dialog = Dialog::SwitchBranch;
        return;
    }
    do_checkout(branch, false);
}

void App::do_checkout(const std::string& branch, bool stash_first) {
    rs->selected_commit.reset();
    repo_op(*this, "Switch branch", [branch, stash_first](git::Repository& repo) {
        if (stash_first) repo.stash_push("");
        repo.checkout_branch(branch);
    }, [this, stash_first] {
        if (stash_first) toast("Your changes were stashed on the previous branch.");
    });
}

void App::create_branch(const std::string& name, std::optional<ObjectId> from) {
    repo_op(*this, "Create branch", [name, from](git::Repository& repo) { repo.create_branch(name, from, true); });
}

void App::delete_branch(const std::string& name, bool remote) {
    auto rname = remote_name();
    auto creds = credentials();
    repo_op(*this, "Delete branch", [name, remote, rname, creds](git::Repository& repo) {
        bool has_upstream = !repo.upstream_of(name).empty();
        repo.delete_branch(name, false);
        if (remote && has_upstream) git::push_delete(repo, rname, name, creds);
    });
}

void App::rename_branch(const std::string& from, const std::string& to) {
    repo_op(*this, "Rename branch", [from, to](git::Repository& repo) { repo.rename_branch(from, to); });
}

void App::merge_into_current(const std::string& ref) {
    auto result = std::make_shared<git::MergeOutcome>();
    repo_op(*this, "Merge", [ref, result](git::Repository& repo) { *result = repo.merge(ref); },
            [this, result, ref] {
                switch (result->kind) {
                    case git::MergeOutcome::UpToDate: toast("This branch is already up to date with " + git::shorten_ref(ref) + "."); break;
                    case git::MergeOutcome::FastForward: toast("Fast-forwarded to " + git::shorten_ref(ref) + "."); break;
                    case git::MergeOutcome::Merged: toast("Merged " + git::shorten_ref(ref) + " into " + rs->branch + "."); break;
                    case git::MergeOutcome::Conflicts: dialog = Dialog::Conflicts; break;
                }
            });
}

void App::undo_rewrite_toast(const std::string& label) {
    Toast t;
    t.text = label + " complete";
    t.until = ui.time + 10;
    t.action_label = "Undo";
    t.action = [this] {
        repo_op(*this, "Undo", [](git::Repository& repo) {
            auto orig = git::try_read_file(repo.gitdir() / "ORIG_HEAD");
            if (!orig) throw git::Error("Nothing to undo.");
            repo.reset_hard(git::ObjectId::from_hex(git::trim(*orig).substr(0, 40)));
        });
    };
    toasts.push_back(t);
}

void App::run_rewrite(const std::string& label, std::function<git::RewriteResult(git::Repository&)> fn) {
    auto result = std::make_shared<git::RewriteResult>();
    repo_op(*this, label, [fn, result](git::Repository& repo) { *result = fn(repo); }, [this, result, label] {
        if (result->conflicts) dialog = Dialog::Conflicts;
        else if (result->new_head) undo_rewrite_toast(label);
    });
}

void App::rebase_onto(const std::string& ref) {
    run_rewrite("Rebase", [ref](git::Repository& repo) { return repo.rebase_onto(ref); });
}

void App::squash_merge(const std::string& ref) {
    auto result = std::make_shared<git::MergeOutcome>();
    repo_op(*this, "Squash and merge", [ref, result](git::Repository& repo) { *result = repo.merge(ref, true); },
            [this, result, ref] {
                if (result->kind == git::MergeOutcome::Conflicts) {
                    dialog = Dialog::Conflicts;
                    rs->summary = "Squashed commit of '" + git::shorten_ref(ref) + "'";
                } else if (result->kind == git::MergeOutcome::UpToDate) toast("Nothing to squash: already up to date.");
                else toast("Squashed " + git::shorten_ref(ref) + " into " + rs->branch + ".");
            });
}

void App::rewrite_continue() {
    run_rewrite("Continue", [](git::Repository& repo) { return repo.rewrite_continue(); });
}

void App::rewrite_abort() {
    repo_op(*this, "Abort", [](git::Repository& repo) { repo.rewrite_abort(); });
}

bool App::commit_is_pushed(const ObjectId& id) const {
    if (!rs) return false;
    if (rs->upstream.empty()) return false;
    for (int i = 0; i < int(rs->history.size()) && i < rs->ahead; i++)
        if (rs->history[size_t(i)].id == id) return false;
    return true;
}

void App::confirm_rewrite(bool pushed, const std::string& what, std::function<void()> go) {
    if (!pushed) { go(); return; }
    rewrite_d.go = std::move(go);
    rewrite_d.what = what;
    dialog = Dialog::ConfirmRewrite;
}

std::vector<ObjectId> App::history_selection_or(const ObjectId& id) const {
    if (rs && rs->history_sel.count(id) && rs->history_sel.size() > 1) return {rs->history_sel.begin(), rs->history_sel.end()};
    return {id};
}

void App::reword_commit(const ObjectId& id, const std::string& message) {
    run_rewrite("Reword", [id, message](git::Repository& repo) { return repo.reword(id, message); });
}

void App::squash_commits(const std::vector<ObjectId>& commits, const ObjectId& into, const std::string& message) {
    run_rewrite("Squash", [commits, into, message](git::Repository& repo) { return repo.squash_commits(commits, into, message); });
}

void App::reorder_commits(const std::vector<ObjectId>& commits, const ObjectId& before) {
    run_rewrite("Reorder", [commits, before](git::Repository& repo) { return repo.reorder(commits, before); });
}

void App::cherry_pick(const std::vector<ObjectId>& commits, const std::string& branch) {
    run_rewrite("Cherry-pick", [commits, branch](git::Repository& repo) { return repo.cherry_pick_onto(commits, branch); });
}

void App::reset_to_commit(const ObjectId& id) {
    repo_op(*this, "Reset to commit", [id](git::Repository& repo) { repo.reset_mixed(id); },
            [this] { toast("Reset complete. Your changes from the undone commits are in the Changes tab."); tab = 0; });
}

void App::delete_tag(const std::string& name, bool remote) {
    auto rname = remote_name();
    auto creds = credentials();
    repo_op(*this, "Delete tag", [name, remote, rname, creds](git::Repository& repo) {
        repo.delete_tag(name);
        if (remote) git::push_delete_ref(repo, rname, "refs/tags/" + name, creds);
    }, [this, name] { toast("Deleted tag " + name); });
}

void App::load_checks(bool force) {
    if (!rs || !rs->github || !rs->head || rs->checks_loading) return;
    auto* acc = account_for_url(rs->remotes.empty() ? "" : rs->remotes[0].url);
    if (!acc) return;
    std::string sha = rs->head->hex();
    if (!force && sha == rs->checks_sha && ui.time - rs->checks_at < 60) return;
    if (rs->upstream.empty()) return;  // unpublished commits have no checks
    rs->checks_loading = true;
    auto a = *acc;
    auto gh = *rs->github;
    auto path = rs->path;
    std::thread([this, a, gh, sha, path] {
        gh::Checks c;
        try { c = gh::list_checks(a, gh.first, gh.second, sha); } catch (...) {}
        jobs.post([this, c, sha, path] {
            if (!rs || rs->path != path) return;
            rs->checks_loading = false;
            rs->checks = c;
            rs->checks_sha = sha;
            rs->checks_at = ui.time;
            int st = int(c.state);
            if (rs->last_check_state == int(gh::Checks::Pending) && st == int(gh::Checks::Failure))
                toast("Checks failed on " + rs->branch, true);
            else if (rs->last_check_state == int(gh::Checks::Pending) && st == int(gh::Checks::Success))
                toast("All checks passed on " + rs->branch);
            rs->last_check_state = st;
        });
    }).detach();
}

void App::rerun_checks() {
    if (!rs || !rs->github) return;
    auto* acc = account_for_url(rs->remotes.empty() ? "" : rs->remotes[0].url);
    if (!acc) return;
    auto a = *acc;
    auto gh = *rs->github;
    auto checks = rs->checks;
    std::thread([this, a, gh, checks] {
        std::string err;
        try { gh::rerun_failed(a, gh.first, gh.second, checks); } catch (const std::exception& e) { err = e.what(); }
        jobs.post([this, err] {
            if (!err.empty()) toast("Couldn't re-run checks: " + err, true);
            else { toast("Re-running failed checks"); if (rs) { rs->checks_at = 0; rs->last_check_state = int(gh::Checks::Pending); } }
        });
    }).detach();
}

void App::load_repo_indicators() {
    std::vector<std::string> paths;
    for (auto& e : repos) paths.push_back(e.path);
    std::thread([this, paths] {
        struct Ind { std::string path; int changes, ahead, behind; bool missing; };
        std::vector<Ind> out;
        for (auto& p : paths) {
            Ind ind{p, 0, 0, 0, false};
            try {
                auto repo = git::Repository::open(git::git_to_path(p));
                ind.changes = int(repo.status().files.size());
                auto br = repo.current_branch();
                auto up = br.empty() ? "" : repo.upstream_of(br);
                if (!up.empty())
                    if (auto h = repo.head(); h)
                        if (auto u = repo.refs().resolve(up)) std::tie(ind.ahead, ind.behind) = repo.ahead_behind(*h, *u);
            } catch (...) { ind.missing = true; }
            out.push_back(ind);
        }
        jobs.post([this, out] {
            for (auto& ind : out)
                for (auto& e : repos)
                    if (e.path == ind.path) {
                        e.changes = ind.changes; e.ahead = ind.ahead; e.behind = ind.behind; e.missing = ind.missing; e.indicators = true;
                    }
        });
    }).detach();
}

void App::load_pr_preview() {
    if (!rs) return;
    pr_d.loading = true;
    pr_d.files.clear();
    pr_d.error.clear();
    auto path = rs->path;
    std::string base = pr_d.base;
    jobs.run("Preview pull request", [this, path, base] {
        std::vector<git::ChangedFile> files;
        int commits = 0;
        std::string err;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            auto h = repo.head();
            auto b = repo.refs().resolve(git::starts_with(base, "refs/") ? base : "refs/heads/" + base);
            if (!b) b = repo.refs().resolve("refs/remotes/" + base);
            if (!h || !b) throw git::Error("Couldn't find the base branch " + base);
            auto mb = repo.merge_base(*h, *b);
            if (!mb) throw git::Error("This branch shares no history with " + base);
            commits = int(repo.commits_between(*mb, *h).size());
            files = repo.tree_changes(repo.odb().read_commit(*mb).tree, repo.odb().read_commit(*h).tree);
        } catch (const std::exception& e) { err = e.what(); }
        auto sh = std::make_shared<std::vector<git::ChangedFile>>(std::move(files));
        jobs.post([this, sh, commits, err] {
            pr_d.loading = false;
            pr_d.files = std::move(*sh);
            pr_d.commits = commits;
            pr_d.error = err;
            if (!pr_d.files.empty()) {
                pr_d.selected = pr_d.files[0].path;
                try {
                    auto repo = git::Repository::open(git::git_to_path(rs->path));
                    pr_d.diff = {};
                    pr_d.diff.path = pr_d.files[0].path;
                    pr_d.diff.diff = repo.commit_file_diff(pr_d.files[0]);
                } catch (...) {}
            }
        });
    });
}

void App::set_zoom(float z) {
    settings.zoom = std::clamp(z, 0.67f, 2.0f);
    r.set_zoom(settings.zoom);
    save_state();
}

void App::check_fork(const std::string& path) {
    auto repo = git::Repository::open(git::git_to_path(path));
    std::string url;
    for (auto& rem : repo.remotes()) if (rem.name == "origin") url = rem.url;
    auto* acc = account_for_url(url);
    if (!acc) return;
    auto g = gh::parse_github_remote(url, acc->host());
    if (!g) return;
    auto a = *acc;
    std::thread([this, a, g, path] {
        try {
            auto info = gh::get_repo(a, g->first, g->second);
            if (!info.fork || info.parent_clone_url.empty()) return;
            jobs.post([this, info, path] {
                fork_d = {info.parent_full_name, info.parent_clone_url, info.parent_default_branch, path, 0};
                dialog = Dialog::ForkChoice;
            });
        } catch (...) {}
    }).detach();
}

void App::abort_merge() {
    repo_op(*this, "Abort merge", [](git::Repository& repo) { repo.abort_merge(); });
}

void App::resolve_conflict(const std::string& path, int stage) {
    repo_op(*this, "Resolve conflict", [path, stage](git::Repository& repo) { repo.resolve_with(path, stage); });
}

// Discarded work goes to the OS trash first, so it can be recovered (like GitHub Desktop).
static void trash_copy(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    auto tmp = fs::temp_directory_path(ec) / ("tributary-discard-" + std::to_string(git::unix_now()));
    fs::create_directories(tmp, ec);
    auto dst = tmp / file.filename();
    fs::copy(file, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (!ec) plat::move_to_trash(git::path_to_git(dst));
    fs::remove_all(tmp, ec);
}

void App::discard(const std::vector<std::string>& paths) {
    repo_op(*this, "Discard changes", [paths](git::Repository& repo) {
        for (auto& p : paths) {
            std::string clean = p.back() == '/' ? p.substr(0, p.size() - 1) : p;
            trash_copy(repo.worktree() / git::git_to_path(clean));
        }
        repo.discard(paths);
    }, [this, paths] { toast(paths.size() == 1 ? "Discarded changes (moved to the trash)" : "Discarded " + std::to_string(paths.size()) + " files (moved to the trash)"); });
}

void App::discard_lines(const std::string& path, const std::vector<bool>& selection) {
    repo_op(*this, "Discard lines", [path, selection](git::Repository& repo) {
        trash_copy(repo.worktree() / git::git_to_path(path));
        repo.discard_lines(path, selection);
    });
}

void App::stash_all() {
    std::string branch = rs ? rs->branch : "";
    repo_op(*this, "Stash changes", [](git::Repository& repo) { repo.stash_push(""); },
            [this] { toast("Your changes are stashed. Restore them from the Changes list."); });
}

void App::stash_restore(bool pop) {
    repo_op(*this, "Restore stash", [pop](git::Repository& repo) { repo.stash_apply(0, pop); },
            [this] { if (rs) rs->viewing_stash = false; });
}

void App::stash_drop() {
    repo_op(*this, "Discard stash", [](git::Repository& repo) { repo.stash_drop(0); },
            [this] { if (rs) rs->viewing_stash = false; });
}

void App::revert_commit(const ObjectId& id) {
    repo_op(*this, "Revert commit", [id](git::Repository& repo) { repo.revert(id); });
}

void App::checkout_pr(const gh::PullRequest& pr) {
    auto remote = remote_name();
    auto creds = credentials();
    std::string local = "pr/" + std::to_string(pr.number);
    std::string head_ref = pr.head_ref;
    repo_op(*this, "Check out pull request", [remote, creds, local, pr](git::Repository& repo) {
        git::fetch_refs(repo, remote, {{"refs/pull/" + std::to_string(pr.number) + "/head", "refs/remotes/" + remote + "/" + local}},
                        creds, {});
        if (!repo.refs().resolve("refs/heads/" + local)) {
            auto id = *repo.refs().resolve("refs/remotes/" + remote + "/" + local);
            repo.create_branch(local, id, false);
        }
        repo.checkout_branch(local);
    });
}

void App::clone_repo(const std::string& url, const std::string& dir) {
    auto creds = credentials();
    git::ProgressFn prog;
    with_progress(*this, prog);
    jobs.run("Cloning", [this, url, dir, creds, prog] {
        std::string err;
        try {
            git::clone(url, git::git_to_path(dir), creds, prog);
        } catch (const std::exception& e) {
            err = e.what();
        }
        jobs.post([this, err, dir, url] {
            if (!err.empty()) { show_error("Clone failed", err); return; }
            toast("Cloned " + url);
            add_repo_path(dir, true);
            check_fork(dir);
        });
    });
}

void App::create_repo() {
    auto d = create_d;
    std::string branch = settings.default_branch.empty() ? "main" : settings.default_branch;
    std::string author = account.name.empty() ? "" : account.name;
    jobs.run("Creating repository", [this, d, branch] {
        std::string err;
        std::string full = d.path + "/" + d.name;
        try {
            if (fs::exists(git::git_to_path(full) / ".git")) throw git::Error("A repository already exists at " + full);
            auto repo = git::Repository::init(git::git_to_path(full), branch);
            std::vector<git::CommitSelection> files;
            if (d.readme) {
                std::string readme = "# " + d.name + "\n";
                if (!d.description.empty()) readme += "\n" + d.description + "\n";
                git::write_file(repo.worktree() / "README.md", readme);
                files.push_back({"README.md", std::nullopt});
            }
            if (d.gitignore) {
                git::write_file(repo.worktree() / ".gitignore", ".DS_Store\nThumbs.db\n*.log\nbuild/\nnode_modules/\ntarget/\n.env\n");
                files.push_back({".gitignore", std::nullopt});
            }
            if (d.mit) {
                std::time_t t = std::time(nullptr);
                std::tm tm{};
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                std::string who = repo.user_name().value_or("The authors");
                git::write_file(repo.worktree() / "LICENSE",
                    "MIT License\n\nCopyright (c) " + std::to_string(tm.tm_year + 1900) + " " + who + "\n\n"
                    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
                    "of this software and associated documentation files (the \"Software\"), to deal\n"
                    "in the Software without restriction, including without limitation the rights\n"
                    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
                    "copies of the Software, and to permit persons to whom the Software is\n"
                    "furnished to do so, subject to the following conditions:\n\n"
                    "The above copyright notice and this permission notice shall be included in all\n"
                    "copies or substantial portions of the Software.\n\n"
                    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
                    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
                    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
                    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
                    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
                    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
                    "SOFTWARE.\n");
                files.push_back({"LICENSE", std::nullopt});
            }
            if (!files.empty() && repo.user_name() && repo.user_email()) repo.commit(files, "Initial commit", "");
        } catch (const std::exception& e) {
            err = e.what();
        }
        jobs.post([this, err, full] {
            if (!err.empty()) { show_error("Couldn't create the repository", err); return; }
            add_repo_path(full, true);
        });
    });
}

void App::publish_repo() {
    if (!rs || !account.valid()) return;
    auto d = publish_d;
    auto acc = account;
    auto creds = credentials();
    git::ProgressFn prog;
    with_progress(*this, prog);
    auto branch = rs->branch;
    repo_op(*this, "Publish repository", [d, acc, creds, prog, branch](git::Repository& repo) {
        auto created = gh::create_repo(acc, d.org, d.name, d.description, d.priv);
        if (repo.config().repo.get("remote.origin.url")) repo.set_remote_url("origin", created.clone_url);
        else repo.add_remote("origin", created.clone_url);
        repo.reload_config();
        if (!branch.empty() && repo.head()) git::push(repo, "origin", branch, creds, prog, true);
    }, [this] { toast("Published to GitHub."); dialog = Dialog::None; });
}

void App::sign_in_with_token(const std::string& token) {
    auto t = git::trim(token);
    if (t.empty()) return;
    std::string endpoint = "https://api.github.com";
    if (ghe_d.signin_tab == 1) {
        std::string u = git::trim(ghe_d.ghe_url);
        if (u.find("://") == std::string::npos) u = "https://" + u;
        while (!u.empty() && u.back() == '/') u.pop_back();
        endpoint = u + "/api/v3";
    }
    jobs.run("Signing in", [this, t, endpoint] {
        std::string err;
        gh::Account acc;
        try { acc = gh::fetch_account(t, endpoint); } catch (const std::exception& e) { err = e.what(); }
        jobs.post([this, err, acc] {
            if (!err.empty()) { show_error("Sign in failed", err); return; }
            if (acc.enterprise()) {
                enterprise = acc;
                plat::secret_set("ghe:" + acc.host() + ":" + acc.login, acc.token);
                save_state();
                toast("Signed in to " + acc.host() + " as " + acc.login);
                if (dialog == Dialog::SignIn) dialog = Dialog::None;
                return;
            }
            account = acc;
            plat::secret_set("github:" + acc.login, acc.token);
            save_state();
            toast("Signed in as " + acc.login);
            if (dialog == Dialog::SignIn) dialog = Dialog::None;
            // offer to fill in git identity if unset
            try {
                auto cfg = git::Config::load(git::global_config_path());
                bool changed = false;
                if (!cfg.get("user.name")) { cfg.set("user.name", acc.name.empty() ? acc.login : acc.name); changed = true; }
                if (!cfg.get("user.email")) { cfg.set("user.email", acc.email); changed = true; }
                if (changed) cfg.save();
            } catch (...) {}
            load_git_identity();
            load_gh_repos(true);
        });
    });
}

void App::start_device_flow() {
    auto cid = settings.oauth_client_id;
    if (cid.empty()) return;
    signin_d.waiting = true;
    signin_d.status = "Contacting GitHub…";
    std::thread([this, cid] {
        try {
            auto dc = gh::start_device_flow(cid);
            jobs.post([this, dc] {
                signin_d.code = dc;
                signin_d.status = "Enter the code below on GitHub.";
                plat::open_url(dc.verification_uri);
            });
            int interval = dc.interval;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(dc.expires_in);
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(interval));
                bool cancelled = false;
                jobs.post([] {});
                if (cancelled) return;
                auto tok = gh::poll_device_flow(cid, dc, interval);
                if (tok) {
                    std::string t = *tok;
                    jobs.post([this, t] {
                        signin_d.waiting = false;
                        sign_in_with_token(t);
                    });
                    return;
                }
            }
            throw std::runtime_error("The sign-in code expired. Please try again.");
        } catch (const std::exception& e) {
            std::string msg = e.what();
            jobs.post([this, msg] {
                signin_d.waiting = false;
                signin_d.status = msg;
            });
        }
    }).detach();
}

void App::sign_out() {
    if (!account.login.empty()) plat::secret_delete("github:" + account.login);
    account = gh::Account{};
    gh_repos.clear();
    save_state();
}

void App::load_gh_repos(bool force) {
    if (!account.valid() || gh_repos_loading) return;
    if (!force && !gh_repos.empty() && ui.time - double(gh_repos_at) < 300) return;
    gh_repos_loading = true;
    auto acc = account;
    std::thread([this, acc] {
        std::vector<gh::RepoSummary> list;
        std::string err;
        try { list = gh::list_repos(acc); } catch (const std::exception& e) { err = e.what(); }
        auto shared = std::make_shared<std::vector<gh::RepoSummary>>(std::move(list));
        jobs.post([this, shared, err] {
            gh_repos_loading = false;
            if (!err.empty()) { toast("Couldn't load your GitHub repositories: " + err, true); return; }
            gh_repos = std::move(*shared);
            gh_repos_at = int64_t(ui.time);
        });
    }).detach();
}

void App::load_prs(bool force) {
    if (!rs || !rs->github || rs->prs_loading) return;
    auto* accp = account_for_url(rs->remotes.empty() ? "" : rs->remotes[0].url);
    if (!accp) return;
    if (!force && rs->prs_loaded_at && ui.time - double(rs->prs_loaded_at) < 120) return;
    rs->prs_loading = true;
    auto acc = *accp;
    auto gh = *rs->github;
    auto path = rs->path;
    std::thread([this, acc, gh, path] {
        std::vector<gh::PullRequest> prs;
        try { prs = gh::list_pull_requests(acc, gh.first, gh.second); } catch (...) {}
        auto shared = std::make_shared<std::vector<gh::PullRequest>>(std::move(prs));
        jobs.post([this, shared, path] {
            if (!rs || rs->path != path) return;
            rs->prs = std::move(*shared);
            rs->prs_loading = false;
            rs->prs_loaded_at = int64_t(ui.time);
        });
    }).detach();
}

// ================================================================ frame
void App::on_event(const plat::Event& ev) {
    plat::Event e = ev;
    if (settings.zoom != 1.0f) { e.x /= settings.zoom; e.y /= settings.zoom; }
    if (e.type == plat::Event::Close) { should_quit = true; return; }
    if (e.type == plat::Event::FocusGained && rs && ui.time - last_status_refresh > 1.0) refresh(false);
    if (e.type == plat::Event::Drop && !e.text.empty()) add_repo_path(e.text, true);
    if (e.type == plat::Event::Command) { run_menu_command(e.command); return; }
    ui.feed(e);
}

// ================================================================ terminal panel
std::string App::terminal_cwd() {
    if (ide.open) {
        for (auto& e : repos) {
            try {
                auto r2 = git::Repository::open(git::git_to_path(e.path));
                for (auto& rm : r2.remotes())
                    if (auto gh2 = gh::parse_github_remote(rm.url, account.host()); gh2 && gh2->first == ide.owner && gh2->second == ide.repo) return e.path;
            } catch (...) {}
        }
        return settings.clone_dir;
    }
    return rs ? rs->path : git::path_to_git(git::home_dir());
}

void App::new_terminal() {
    auto s = std::make_unique<ui::TerminalSession>();
    s->cwd = git::git_to_path(terminal_cwd()).string();
    auto sh = plat::default_shell();
    std::string base = sh.empty() ? "shell" : sh[0];
    auto sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".exe") base.resize(base.size() - 4);
    s->name = base;
    term_panel.tabs.push_back(std::move(s));
    term_panel.active = int(term_panel.tabs.size()) - 1;
    term_panel.open = true;
}

void App::toggle_terminal() {
    if (!term_panel.open && term_panel.tabs.empty()) new_terminal();
    else term_panel.open = !term_panel.open;
    if (term_panel.open) ui.set_focus(ui.id("term", term_panel.active));
}

Rect App::draw_terminal_panel(const Rect& content) {
    auto& T = ui.theme;
    if (term_panel.tabs.empty()) { term_panel.open = false; return content; }
    float h = std::clamp(term_panel.height, 120.f, std::max(140.f, content.h - 120));
    Rect panel{content.x, content.bottom() - h, content.w, h};
    // resize by dragging the top edge
    Rect grip{panel.x, panel.y - 3, panel.w, 6};
    auto gid = ui.id("term-grip");
    if (ui.hovered(grip)) ui.set_cursor(plat::Cursor::ResizeV);
    if (ui.pressed(gid, grip)) {}
    if (ui.is_active(gid)) {
        ui.set_cursor(plat::Cursor::ResizeV);
        if (ui.mouse_down[0]) term_panel.height = std::clamp(content.bottom() - ui.my, 120.f, content.h - 120);
        else ui.clear_active();
    }
    r.rect(panel, T.bg);
    r.line_h(panel.x, panel.right(), panel.y, T.border);
    // header: tabs, new, kill, close (like VS Code's TERMINAL panel)
    Rect head{panel.x, panel.y + 1, panel.w, 30};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom(), T.border);
    float x = head.x + 10;
    x += r.draw_text_in(FontId::Bold, 11, {x, head.y, 100, head.h}, "TERMINAL", T.text_muted) + 14;
    int to_close = -1;
    for (size_t i = 0; i < term_panel.tabs.size(); i++) {
        auto& s = *term_panel.tabs[i];
        std::string label = s.term.title().empty() ? s.name : s.term.title();
        if (label.size() > 28) label = label.substr(0, 27) + "…";
        float w = ui.text_w(FontId::Regular, 12, label) + 50;
        Rect tr{x, head.y + 3, w, head.h - 6};
        bool act = int(i) == term_panel.active;
        if (act) r.rect(tr, T.bg, 5, 1, T.border);
        else if (ui.hovered(tr)) r.rect(tr, T.row_hover, 5);
        r.icon("terminal", tr.x + 7, tr.y + 4, 16, s.exited ? T.text_muted : T.text);
        r.draw_text_in(FontId::Regular, 12, {tr.x + 28, tr.y, w - 46, tr.h}, label, s.exited ? T.text_muted : T.text);
        Rect xr{tr.right() - 20, tr.y + 4, 16, 16};
        if (act || ui.hovered(tr)) r.icon("x", xr.x, xr.y, 16, ui.hovered(xr) ? T.text : T.text_muted);
        if (ui.clicked(ui.id("term-x", int64_t(i)), xr)) to_close = int(i);
        else if (ui.clicked(ui.id("term-tab", int64_t(i)), tr)) { term_panel.active = int(i); ui.set_focus(ui.id("term", int64_t(i))); }
        x += w + 4;
    }
    Rect plus{x + 2, head.y + 4, 22, 22};
    if (ui::icon_button(ui, ui.id("term-new"), plus, "plus", "New terminal")) { new_terminal(); ui.set_focus(ui.id("term", term_panel.active)); }
    Rect closep{head.right() - 30, head.y + 4, 22, 22};
    if (ui::icon_button(ui, ui.id("term-hide"), closep, "chevron-down", "Hide panel (Ctrl+`)")) term_panel.open = false;
    if (to_close >= 0) {
        term_panel.tabs.erase(term_panel.tabs.begin() + to_close);
        term_panel.active = std::clamp(term_panel.active - (to_close < term_panel.active ? 1 : 0), 0, std::max(0, int(term_panel.tabs.size()) - 1));
        if (term_panel.tabs.empty()) { term_panel.open = false; return content; }
    }
    Rect body{panel.x, head.bottom() + 1, panel.w, panel.bottom() - head.bottom() - 1};
    auto& s = *term_panel.tabs[size_t(term_panel.active)];
    auto id = ui.id("term", term_panel.active);
    terminal_focus_id_ = id;
    ui::terminal_view(ui, id, body, s, [this] { win.wake(); });
    return {content.x, content.y, content.w, panel.y - content.y};
}

void App::frame() {
    jobs.drain();
    if (!r.begin_frame(ui.theme.bg)) {
        ui.begin_frame();
        ui.end_frame();
        return;
    }
    ui.begin_frame();
    for (auto& k : injected_keys) ui.keys.push_back(k);
    injected_keys.clear();
    auto& T = ui.theme;
    float W = r.width(), H = r.height();
    // periodic background refresh while focused (cheap: stat-based status)
    if (rs && ui.window_focused && !jobs.busy() && ui.time - last_status_refresh > 8.0) {
        static int n = 0;
        refresh(++n % 4 == 0);  // every 4th background refresh also reloads branches & stashes
    }
    // auto-fetch every 15 minutes like GitHub Desktop
    if (rs && !rs->remotes.empty() && account.valid() && ui.time - last_auto_fetch > 900 && !jobs.busy() && rs->loaded) {
        last_auto_fetch = ui.time;
        if (last_auto_fetch > 5) fetch();
    }
    if (rs && rs->github && ui.window_focused) {
        load_prs();
        if (ui.time - rs->checks_at > (rs->checks.state == gh::Checks::Pending ? 20 : 120)) load_checks();
    }
    handle_shortcuts();
    sync_native_menu();
    // title strip: macOS draws its own behind the traffic lights (--darwin-title-bar-height),
    // Windows/Linux get Desktop's menu strip (--win32-title-bar-height)
    float top = win.has_native_menu() ? win.titlebar_height() : 28;
    Rect strip{0, 0, W, top};
    Rect toolbar{0, top, W, 50};
    Rect body{0, top + 50, W, H - top - 50};
    // the terminal panel spans the content area, like VS Code's panel
    terminal_focus_id_ = 0;
    if (term_panel.open && (ide.open || (rs && !repos.empty()))) {
        float sw = std::clamp(settings.sidebar_width, 220.f, std::max(260.f, W * 0.5f));
        Rect content{sw + 1, body.y, W - sw - 1, body.h};
        Rect above = draw_terminal_panel(content);
        panel_inset_ = content.h - above.h;
    } else {
        panel_inset_ = 0;
    }
    if (ide.open) {
        draw_ide(body);
    } else if (repos.empty() || !rs) {
        draw_welcome(body);
    } else {
        float sw = std::clamp(settings.sidebar_width, 220.f, std::max(260.f, W * 0.5f));
        Rect side{0, body.y, sw, body.h};
        Rect main{sw + 1, body.y, W - sw - 1, body.h - panel_inset_};
        draw_sidebar(side);
        float nw = ui::splitter(ui, ui.id("sidebar-split"), {sw, body.y, 1, body.h}, sw, 220, std::max(260.f, W * 0.5f));
        if (nw != sw) { settings.sidebar_width = nw; }
        if (ui.mouse_released[0] && nw != 300) save_state();
        r.push_clip(main);
        draw_main(main);
        r.pop_clip();
    }
    if (ide.open) {
        draw_ide_toolbar(toolbar);
        draw_context_menu();
    } else {
        draw_toolbar(toolbar);
    }
    pump_avatars();
    if (win.has_native_menu()) {
        // linear-gradient(to bottom, #3b3f46 0%, #2b2e33 100%)
        for (int i = 0; i < int(top); i++) {
            float t = float(i) / std::max(1.f, top - 1);
            r.rect({0, float(i), W, 1}, Color::hex(0x3b3f46).mix(Color::hex(0x2b2e33), t));
        }
        if (rs) r.draw_text_in(FontId::Regular, 12, strip, rs->name, Color::hex(0xd1d5da), Align::Center);
    } else {
        draw_menu_bar(strip);
    }
    draw_dialogs();
    draw_toasts();
    (void)T;
    ui.end_frame();
    r.end_frame();
}

}  // namespace app
