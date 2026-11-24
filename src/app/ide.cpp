// SPDX-License-Identifier: MIT
// Editing on GitHub: browse a repository's files over the API, edit any number of
// them in the built-in code editor, and submit everything as one pull request.
#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

#include "app.hpp"
#include "../gfx/image.hpp"

namespace app {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using plat::Key;
using ui::Id;

static bool looks_binary(const std::string& s) {
    size_t n = std::min<size_t>(s.size(), 8000);
    for (size_t i = 0; i < n; i++) if (s[i] == 0) return true;
    // invalid UTF-8 counts as binary
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) return true;
        for (size_t k = 1; k < len; k++) if (((unsigned char)s[i + k] >> 6) != 2) return true;
        i += len;
    }
    return false;
}

static std::string base_name(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
static std::string dir_name(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

static const char* change_icon(git::Change c) {
    switch (c) {
        case git::Change::Added: return "diff-added";
        case git::Change::Deleted: return "diff-removed";
        default: return "diff-modified";
    }
}
static Color change_color(const ui::Theme& T, git::Change c) {
    switch (c) {
        case git::Change::Added: return T.success;
        case git::Change::Deleted: return T.danger;
        default: return T.modified;
    }
}

// ================================================================ state
void App::open_ide(const std::string& owner, const std::string& repo, const std::string& branch) {
    if (!account.valid()) { ghe_d.signin_tab = 0; dialog = Dialog::SignIn; toast("Sign in to GitHub to edit repositories."); return; }
    ide = IdeState{};
    ide.open = true;
    ide.owner = owner;
    ide.repo = repo;
    ide.base = branch;
    ide.loading = true;
    uint64_t gen = ++ide_generation_;
    ide.generation = gen;
    auto acc = account;
    auto alive = alive_;
    std::thread([this, acc, owner, repo, branch, gen, alive] {
        std::string err, base = branch, def;
        gh::RemoteTree tree;
        std::vector<std::string> branches;
        bool push = true;
        try {
            auto info = gh::get_repo(acc, owner, repo);
            def = info.default_branch.empty() ? "main" : info.default_branch;
            if (base.empty()) base = def;
            tree = gh::get_tree(acc, owner, repo, base);
            try { branches = gh::list_branches(acc, owner, repo); } catch (...) {}
            try { push = gh::can_push(acc, owner, repo); } catch (...) {}
        } catch (const std::exception& e) { err = e.what(); }
        if (!alive->load()) return;
        jobs.post([this, gen, err, base, def, tree, branches, push] {
            if (ide.generation != gen) return;
            ide.loading = false;
            ide.error = err;
            ide.base = base;
            ide.default_branch = def;
            ide.commit_sha = tree.commit_sha;
            ide.truncated = tree.truncated;
            ide.branches = branches;
            ide.can_push = push;
            for (auto& f : tree.files) {
                if (f.mode == "160000") continue;
                IdeFile x;
                x.path = f.path;
                x.base_sha = f.sha;
                x.mode = f.mode;
                x.size = f.size;
                ide.files[f.path] = std::move(x);
            }
            ide.tree_ver++;
            // open the README like github.com does
            for (const char* rd : {"README.md", "readme.md", "README", "README.rst", "README.txt"})
                if (ide.files.count(rd)) { ide_open_file(rd, false); break; }
        });
    }).detach();
}

void App::ide_load(const std::string& path) {
    auto it = ide.files.find(path);
    if (it == ide.files.end()) return;
    auto& f = it->second;
    if (f.state != IdeFile::NotLoaded) return;
    std::string sha = f.added ? f.copy_from_sha : f.base_sha;
    if (sha.empty()) {  // brand-new file
        f.state = IdeFile::Loaded;
        f.doc.set_text("", path);
        f.doc.final_newline = true;
        return;
    }
    if (f.size > 8 * 1024 * 1024) { f.state = IdeFile::Failed; f.error = "This file is too large to edit here (over 8 MB)."; return; }
    f.state = IdeFile::Loading;
    auto acc = account;
    auto alive = alive_;
    std::string owner = ide.owner, repo = ide.repo;
    uint64_t gen = ide.generation;
    std::thread([this, acc, owner, repo, sha, path, gen, alive] {
        std::string data, err;
        try { data = gh::get_blob(acc, owner, repo, sha); } catch (const std::exception& e) { err = e.what(); }
        auto img = std::make_shared<gfx::Image>();
        bool bin = err.empty() && looks_binary(data);
        if (bin && gfx::is_image_data(data)) { try { *img = gfx::decode_image(data); } catch (...) {} }
        if (!alive->load()) return;
        jobs.post([this, path, gen, data, err, bin, img] {
            if (ide.generation != gen) return;
            auto it2 = ide.files.find(path);
            if (it2 == ide.files.end()) return;
            auto& f2 = it2->second;
            if (!err.empty()) { f2.state = IdeFile::Failed; f2.error = err; return; }
            f2.state = IdeFile::Loaded;
            f2.binary = bin;
            if (bin) {
                f2.raw = data;
                if (img->valid()) f2.image = img;
            } else {
                f2.doc.set_text(data, path);
                f2.original = f2.added ? "" : data;
            }
            f2.checked_version = ~0ull;
        });
    }).detach();
}

void App::ide_open_file(const std::string& path, bool focus) {
    if (!ide.files.count(path)) return;
    if (std::find(ide.tabs.begin(), ide.tabs.end(), path) == ide.tabs.end()) ide.tabs.push_back(path);
    ide.active = path;
    ide.tree_sel = path;
    ide_load(path);
    // reveal it in the tree
    for (std::string d = dir_name(path); !d.empty(); d = dir_name(d)) ide.expanded.insert(d);
    ide.tree_ver++;
    if (focus) ui.set_focus(ui.id("ide-editor"));
}

void App::ide_close_tab(const std::string& path) {
    auto it = std::find(ide.tabs.begin(), ide.tabs.end(), path);
    if (it == ide.tabs.end()) return;
    size_t idx = size_t(it - ide.tabs.begin());
    ide.tabs.erase(it);
    if (ide.active == path) ide.active = ide.tabs.empty() ? "" : ide.tabs[std::min(idx, ide.tabs.size() - 1)];
}

std::vector<std::pair<std::string, git::Change>> App::ide_changes() {
    std::vector<std::pair<std::string, git::Change>> out;
    for (auto& [p, f] : ide.files) {
        if (f.added && !f.deleted) { out.push_back({p, git::Change::Added}); continue; }
        if (f.deleted && !f.added) { out.push_back({p, git::Change::Deleted}); continue; }
        if (f.state == IdeFile::Loaded && !f.binary) {
            if (f.checked_version != f.doc.version) {
                f.modified = f.doc.text() != f.original;
                f.checked_version = f.doc.version;
            }
            if (f.modified) out.push_back({p, git::Change::Modified});
        }
    }
    return out;
}

void App::ide_discard(const std::string& path) {
    auto it = ide.files.find(path);
    if (it == ide.files.end()) return;
    auto& f = it->second;
    if (f.added) {
        std::string from = f.renamed_from;
        ide_close_tab(path);
        ide.files.erase(it);
        if (!from.empty() && ide.files.count(from)) ide.files[from].deleted = false;
    } else if (f.deleted) {
        f.deleted = false;
    } else if (f.state == IdeFile::Loaded && !f.binary) {
        auto keep = f.doc.scroll_y;
        f.doc.set_text(f.original, path);
        f.doc.scroll_y = keep;
    }
    if (ide.change_sel == path) ide.change_sel.clear();
    ide.change_diff_ver = ~0ull;
    ide.tree_ver++;
}

void App::ide_delete(const std::string& path) {
    auto it = ide.files.find(path);
    if (it == ide.files.end()) return;
    ide_close_tab(path);
    if (it->second.added) ide.files.erase(it);
    else it->second.deleted = true;
    ide.tree_ver++;
}

void App::ide_new_file(const std::string& path) {
    IdeFile f;
    f.path = path;
    f.added = true;
    f.state = IdeFile::Loaded;
    f.doc.set_text("", path);
    f.doc.final_newline = true;
    ide.files[path] = std::move(f);
    ide.tree_ver++;
    ide_open_file(path);
}

void App::ide_rename(const std::string& from, const std::string& to) {
    auto it = ide.files.find(from);
    if (it == ide.files.end() || from == to) return;
    IdeFile nf = it->second;
    nf.path = to;
    nf.added = true;
    nf.deleted = false;
    nf.original.clear();
    nf.checked_version = ~0ull;
    if (it->second.added) {
        nf.renamed_from = it->second.renamed_from;
        ide.files.erase(it);
    } else {
        nf.renamed_from = from;
        nf.copy_from_sha = it->second.base_sha;
        it->second.deleted = true;
    }
    nf.base_sha.clear();
    if (nf.state == IdeFile::Loaded && !nf.binary) nf.doc.lang = ui::lang_for_path(to), nf.doc.version++;
    ide.files[to] = std::move(nf);
    for (auto& t : ide.tabs) if (t == from) t = to;
    if (ide.active == from) ide.active = to;
    ide.tree_ver++;
}

void App::ide_guard(std::function<void()> fn) {
    if (ide.open && !ide_changes().empty()) {
        ide_after_discard = std::move(fn);
        dialog = Dialog::IdeConfirmDiscard;
    } else fn();
}

void App::ide_submit() {
    auto changes = ide_changes();
    if (changes.empty() || ide_pr_d.submitting) return;
    for (auto& [p, kind] : changes) {
        auto& f = ide.files[p];
        if (f.state != IdeFile::Loaded || f.binary) continue;
        for (auto& l : f.doc.lines)
            if (l.rfind("<<<<<<< ", 0) == 0 || l.rfind(">>>>>>> ", 0) == 0) {
                ide_pr_d.error = p + " still has conflict markers (<<<<<<< / >>>>>>>). Resolve them first.";
                return;
            }
    }
    // collect edits on the UI thread; renamed-but-unopened files are fetched on the worker
    struct Pending { gh::FileEdit edit; std::string fetch_sha, original; bool modified = false; };
    std::vector<Pending> pend;
    for (auto& [p, kind] : changes) {
        auto& f = ide.files[p];
        Pending pe;
        pe.edit.path = p;
        pe.edit.mode = f.mode.empty() ? "100644" : f.mode;
        if (kind == git::Change::Deleted) { pe.edit.base_sha = f.base_sha; }
        else if (kind == git::Change::Added) {
            if (f.state == IdeFile::Loaded) pe.edit.content = f.binary ? f.raw : f.doc.text();
            else pe.fetch_sha = f.copy_from_sha;
        } else {
            pe.edit.base_sha = f.base_sha;
            pe.edit.content = f.doc.text();
            pe.original = f.original;
            pe.modified = true;
        }
        pend.push_back(std::move(pe));
    }
    gh::PrRequest rq;
    rq.owner = ide.owner;
    rq.repo = ide.repo;
    rq.base_branch = ide.base;
    rq.head_branch = git::trim(ide_pr_d.branch);
    rq.title = git::trim(ide_pr_d.title);
    rq.body = ide_pr_d.body;
    rq.commit_message = rq.title + (git::trim(rq.body).empty() ? "" : "\n\n" + git::trim(rq.body)) + "\n";
    rq.draft = ide_pr_d.draft;
    ide_pr_d.submitting = true;
    ide_pr_d.error.clear();
    ide_pr_d.progress = "Starting…";
    auto acc = account;
    auto alive = alive_;
    std::thread([this, acc, rq, pend, alive]() mutable {
        std::string err;
        gh::PrResult res;
        // files that changed on the base branch since they were opened: three-way merge
        // their change into ours; on a real conflict, hand the markers back to the editor
        struct Update { std::string path, text, original, sha; bool conflict; };
        std::vector<Update> updates;
        try {
            if (alive->load()) jobs.post([this] { ide_pr_d.progress = "Checking for newer changes…"; });
            auto cur = gh::get_tree(acc, rq.owner, rq.repo, rq.base_branch);
            std::map<std::string, std::string> now;
            for (auto& f : cur.files) now[f.path] = f.sha;
            std::vector<std::string> conflicted;
            for (auto& pe : pend) {
                if (!pe.modified) continue;
                auto it = now.find(pe.edit.path);
                if (it == now.end()) throw std::runtime_error(pe.edit.path + " was deleted on " + rq.base_branch + " after you opened it.");
                if (it->second == pe.edit.base_sha) continue;
                std::string theirs = gh::get_blob(acc, rq.owner, rq.repo, it->second);
                auto m = git::merge3(pe.original, *pe.edit.content, theirs, "your edit", rq.base_branch);
                updates.push_back({pe.edit.path, m.text, theirs, it->second, m.conflict});
                if (m.conflict) conflicted.push_back(pe.edit.path);
                pe.edit.content = m.text;
                pe.edit.base_sha = it->second;
            }
            if (!conflicted.empty()) {
                std::string list;
                for (auto& c : conflicted) list += (list.empty() ? "" : ", ") + c;
                throw std::runtime_error("Your edits conflict with newer changes on " + rq.base_branch + " in " + list +
                                         ". The conflict markers are in the editor now; fix them and submit again.");
            }
            for (auto& pe : pend) {
                if (!pe.fetch_sha.empty()) pe.edit.content = gh::get_blob(acc, rq.owner, rq.repo, pe.fetch_sha);
                rq.edits.push_back(pe.edit);
            }
            res = gh::submit_pull_request(acc, rq, [this, alive](const std::string& s) {
                if (alive->load()) jobs.post([this, s] { ide_pr_d.progress = s; });
            });
        } catch (const std::exception& e) { err = e.what(); }
        if (!alive->load()) return;
        jobs.post([this, updates] {
            // bring merged (or conflicted) text into the editor, rebased on the newer blob
            for (auto& u : updates) {
                auto it = ide.files.find(u.path);
                if (it == ide.files.end()) continue;
                auto& f = it->second;
                auto cur = f.doc.cursor;
                f.doc.undo.push_back({f.doc.lines, f.doc.cursor, f.doc.anchor});
                auto undo = std::move(f.doc.undo);
                f.doc.set_text(u.text, u.path);
                f.doc.undo = std::move(undo);
                f.doc.cursor = f.doc.anchor = cur;
                f.original = u.original;
                f.base_sha = u.sha;
                f.checked_version = ~0ull;
            }
            size_t clean = 0;
            for (auto& u : updates) if (!u.conflict) clean++;
            if (clean && clean == updates.size())
                toast("Merged newer changes from " + ide.base + " into " + std::to_string(clean) + " of your files.");
        });
        jobs.post([this, err, res, rq] {
            ide_pr_d.submitting = false;
            if (!err.empty()) { ide_pr_d.error = err; return; }
            dialog = Dialog::None;
            Toast t;
            t.text = "Opened pull request #" + std::to_string(res.number) + (res.forked ? " from your fork" : "") + ".";
            t.until = ui.time + 10;
            std::string url = res.html_url;
            t.action_label = "View on GitHub";
            t.action = [url] { plat::open_url(url); };
            toasts.push_back(t);
            // start fresh from the base branch; the PR has the edits now
            open_ide(rq.owner, rq.repo, rq.base_branch);
        });
    }).detach();
}

// ================================================================ toolbar
static void ide_tb_button(App& a, const Rect& rc, const char* icon, const std::string& caption, const std::string& title,
                          bool dropdown, bool open, bool enabled = true) {
    auto& T = a.ui.theme;
    Rect inner{rc.x, rc.y, rc.w - 1, rc.h};
    bool hov = enabled && a.ui.hovered(rc);
    if (open) a.r.rect(inner, T.bg);
    else if (hov) a.r.rect(inner, T.toolbar_hover);
    Color fg = open ? T.text : T.toolbar_text, dim = open ? T.text_muted : T.toolbar_text_dim;
    if (!enabled) { fg = fg.with_alpha(0.5f); dim = dim.with_alpha(0.5f); }
    a.r.icon(icon, rc.x + 10, rc.y + (rc.h - 16) / 2, 16, fg);
    float tx = rc.x + 36, tw = rc.w - 36 - (dropdown ? 36 : 10);
    a.r.draw_text_in(FontId::Regular, 11, {tx, rc.y + 8, tw, 16}, a.r.ellipsize(FontId::Regular, 11, caption, tw), dim);
    a.r.draw_text_in(FontId::Bold, 12, {tx, rc.y + 24, tw, 18}, a.r.ellipsize(FontId::Bold, 12, title, tw), fg);
    if (dropdown) a.r.icon("triangle-down", rc.right() - 27, rc.y + (rc.h - 16) / 2, 16, fg);
    a.r.line_v(rc.right() - 1, rc.y, rc.bottom(), T.toolbar_border);
    if (hov) a.ui.set_cursor(plat::Cursor::Hand);
}

void App::draw_ide_toolbar(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg_toolbar);
    r.line_h(rc.x, rc.right(), rc.bottom() - 1, T.dark ? T.border : T.bg_toolbar);
    float sec = std::clamp(settings.sidebar_width, 220.f, std::max(260.f, rc.w * 0.5f));
    Rect repo_r{0, rc.y, sec + 1, rc.h - 1}, branch_r{sec + 1, rc.y, 230, rc.h - 1}, pr_r{sec + 231, rc.y, 230, rc.h - 1};
    ide_tb_button(*this, repo_r, "repo", "Editing on GitHub", ide.owner + "/" + ide.repo, true, false);
    if (ui.clicked(ui.id("ide-tb-repo"), repo_r)) {
        ide_open_d = {};
        dialog = Dialog::IdeOpen;
        load_gh_repos();
    }
    auto bpid = ui.id("ide-branch-popup");
    ide_tb_button(*this, branch_r, "git-branch", "Base branch", ide.base.empty() ? "…" : ide.base, true, ui.popup_open(bpid));
    if (ui.clicked(ui.id("ide-tb-branch"), branch_r)) { if (ui.popup_open(bpid)) ui.close_popup(); else ui.open_popup(bpid); }
    auto changes = ide_changes();
    std::string cap = changes.empty() ? "No changes yet" : std::to_string(changes.size()) + " changed file" + (changes.size() == 1 ? "" : "s");
    ide_tb_button(*this, pr_r, "git-pull-request", cap, "Create pull request", false, false, !changes.empty());
    if (!changes.empty() && ui.clicked(ui.id("ide-tb-pr"), pr_r)) {
        ide_pr_d = {};
        // GitHub's own naming for web edits: <login>-patch-N
        for (int n = 1;; n++) {
            std::string b = account.login + "-patch-" + std::to_string(n);
            if (std::find(ide.branches.begin(), ide.branches.end(), b) == ide.branches.end()) { ide_pr_d.branch = b; break; }
        }
        if (changes.size() == 1) {
            auto& [p, k] = changes[0];
            ide_pr_d.title = std::string(k == git::Change::Added ? "Create " : k == git::Change::Deleted ? "Delete " : "Update ") + base_name(p);
        } else {
            ide_pr_d.title = "Update " + std::to_string(changes.size()) + " files";
        }
        dialog = Dialog::IdeSubmit;
    }
    // close button on the right
    Rect close_r{rc.right() - 140, rc.y, 140, rc.h - 1};
    bool ch = ui.hovered(close_r);
    if (ch) { r.rect(close_r, T.toolbar_hover); ui.set_cursor(plat::Cursor::Hand); }
    r.line_v(close_r.x, close_r.y, close_r.bottom(), T.toolbar_border);
    r.icon("x", close_r.x + 14, close_r.y + (close_r.h - 16) / 2, 16, T.toolbar_text);
    r.draw_text_in(FontId::Bold, 12, {close_r.x + 38, close_r.y, close_r.w - 40, close_r.h}, "Close editor", T.toolbar_text);
    if (ui.clicked(ui.id("ide-close"), close_r)) ide_guard([this] { ide = IdeState{}; });
    // assistant toggle, left of Close editor
    Rect ai_r{close_r.x - 130, rc.y, 130, rc.h - 1};
    bool ah = ui.hovered(ai_r);
    if (agent.open) r.rect(ai_r, T.bg);
    else if (ah) { r.rect(ai_r, T.toolbar_hover); }
    if (ah) ui.set_cursor(plat::Cursor::Hand);
    r.line_v(ai_r.x, ai_r.y, ai_r.bottom(), T.toolbar_border);
    Color afg = agent.open ? T.text : T.toolbar_text;
    r.icon("sparkle-fill", ai_r.x + 14, ai_r.y + (ai_r.h - 16) / 2, 16, agent.open ? T.accent_emphasis : afg);
    r.draw_text_in(FontId::Bold, 12, {ai_r.x + 38, ai_r.y, ai_r.w - 40, ai_r.h}, "Assistant", afg);
    if (ui.clicked(ui.id("ide-ai"), ai_r)) {
        agent.open = !agent.open;
        if (agent.open) agent_load_models(settings.ai_provider);
    }
    // branch popup
    Rect bp{branch_r.x, rc.bottom(), 300, std::min(420.f, r.height() - rc.bottom())};
    if (ui::begin_popup(ui, bpid, bp, T.bg, 0.f)) {
        float y = bp.y + 4;
        int i = 0;
        float off = ui::begin_scroll(ui, ui.id("ide-branch-list"), bp);
        y -= off;
        for (auto& b : ide.branches) {
            Rect row{bp.x, y, bp.w, 30};
            bool cur = b == ide.base;
            if (ui::menu_item(ui, ui.id("ide-br", i++), row, b, "", true, cur ? "check" : "git-branch") && !cur) {
                std::string nb = b;
                ide_guard([this, nb] { open_ide(ide.owner, ide.repo, nb); });
            }
            y += 30;
        }
        ui::end_scroll(ui, ui.id("ide-branch-list"), bp, y + off - bp.y + 4);
        ui::end_popup(ui);
    }
}

// ================================================================ main area
void App::draw_ide(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg);
    if (ide.loading) {
        ui::spinner(ui, rc.x + rc.w / 2, rc.y + rc.h / 3, 9, T.text_muted);
        r.draw_text_in(FontId::Regular, 12, {rc.x, rc.y + rc.h / 3 + 24, rc.w, 18}, "Loading " + ide.owner + "/" + ide.repo + "…", T.text_muted, Align::Center);
        return;
    }
    if (!ide.error.empty()) {
        r.icon("alert", rc.x + rc.w / 2 - 12, rc.y + rc.h / 3 - 40, 24, T.attention);
        r.draw_text_in(FontId::Bold, 14, {rc.x, rc.y + rc.h / 3, rc.w, 22}, "Couldn't open " + ide.owner + "/" + ide.repo, T.text, Align::Center);
        r.draw_text_in(FontId::Regular, 12, {rc.x, rc.y + rc.h / 3 + 26, rc.w, 18}, ide.error, T.text_muted, Align::Center);
        if (ui::button(ui, ui.id("ide-retry"), {rc.x + rc.w / 2 - 60, rc.y + rc.h / 3 + 60, 120, 25}, "Try again", ui::BtnStyle::Primary))
            open_ide(ide.owner, ide.repo, ide.base);
        return;
    }
    float sw = std::clamp(settings.sidebar_width, 220.f, std::max(260.f, rc.w * 0.5f));
    Rect side{rc.x, rc.y, sw, rc.h};
    Rect main{rc.x + sw + 1, rc.y, rc.w - sw - 1, rc.h - panel_inset_};
    if (agent.open) {
        float aw = std::clamp(settings.ai_width, 300.f, std::max(300.f, main.w - 300));
        Rect ap{main.right() - aw, rc.y, aw, rc.h - panel_inset_};
        main.w -= aw;
        r.push_clip(ap);
        draw_agent_panel(ap);
        r.pop_clip();
        float nw = ui::splitter(ui, ui.id("ai-split"), {ap.x, ap.y, 1, ap.h}, ap.x, main.x + 300, rc.right() - 300);
        if (nw != ap.x) settings.ai_width = rc.right() - nw;
    }
    r.line_v(side.right(), side.y, side.bottom(), T.border);
    auto changes = ide_changes();
    int nt = ui::tabs(ui, ui.id("ide-side-tabs"), {side.x, side.y, side.w, 29}, {"Files", "Changes"}, ide.side_tab,
                      {"", changes.empty() ? "" : std::to_string(changes.size())});
    ide.side_tab = nt;
    Rect body{side.x, side.y + 29, side.w, side.h - 29};
    if (ide.side_tab == 0) draw_ide_tree(body);
    else draw_ide_changes(body);
    float nw = ui::splitter(ui, ui.id("ide-split"), {sw, rc.y, 1, rc.h}, sw, 220, std::max(260.f, rc.w * 0.5f));
    if (nw != sw) settings.sidebar_width = nw;
    if (ide.side_tab == 1 && !ide.change_sel.empty() && ide.files.count(ide.change_sel)) {
        auto& f = ide.files[ide.change_sel];
        uint64_t ver = f.doc.version * 4 + (f.deleted ? 1 : 0) + (f.added ? 2 : 0);
        if (ide.change_diff_ver != ver) {
            ide.change_diff_ver = ver;
            ide.change_diff = {};
            ide.change_diff.path = ide.change_sel;
            if (f.binary) ide.change_diff.diff.binary = true;
            else {
                std::string now = f.deleted ? "" : f.state == IdeFile::Loaded ? f.doc.text() : "";
                std::string was = f.added ? "" : f.original;
                ide.change_diff.diff = git::diff_text(was, now, 3);
            }
        }
        std::string extra = !f.renamed_from.empty() ? "Renamed from " + f.renamed_from : "";
        r.push_clip(main);
        draw_diff(main, ide.change_diff, false, ide.change_sel, extra);
        r.pop_clip();
        return;
    }
    r.push_clip(main);
    draw_ide_editor(main);
    r.pop_clip();
}

void App::draw_ide_tree(const Rect& rc) {
    auto& T = ui.theme;
    // header: filter + new file
    Rect head{rc.x, rc.y, rc.w, 45};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    ui::TextFieldOpts fo;
    fo.placeholder = "Go to file";
    fo.icon = "search";
    std::string before = ide.filter;
    ui::text_field(ui, ui.id("ide-filter"), {head.x + 10, head.y + 10, head.w - 50, 25}, ide.filter, fo);
    if (before != ide.filter) ide.tree_ver++;
    if (ui::icon_button(ui, ui.id("ide-new"), {head.right() - 35, head.y + 10, 25, 25}, "plus", "New file")) {
        ide_name_d = {};
        std::string d = ide.tree_sel.empty() ? "" : (ide.files.count(ide.tree_sel) ? dir_name(ide.tree_sel) : ide.tree_sel);
        ide_name_d.path = d.empty() ? "" : d + "/";
        dialog = Dialog::IdeNewFile;
    }
    Rect list{rc.x, head.bottom(), rc.w, rc.h - head.h};
    // rebuild the visible node list when something changed
    if (ide.tree_built != ide.tree_ver) {
        ide.tree_built = ide.tree_ver;
        ide.nodes.clear();
        std::string f = git::to_lower(git::trim(ide.filter));
        if (!f.empty()) {
            // flat, fuzzy-ish: every query character in order
            std::vector<std::pair<int, std::string>> hits;
            for (auto& [p, file] : ide.files) {
                if (file.deleted) continue;
                std::string lp = git::to_lower(p);
                size_t k = 0;
                int gaps = 0;
                size_t last = 0;
                for (size_t i = 0; i < lp.size() && k < f.size(); i++)
                    if (lp[i] == f[k]) { if (k && i != last + 1) gaps++; last = i; k++; }
                if (k == f.size()) hits.push_back({gaps * 10 + int(p.size()) / 8 - (git::to_lower(base_name(p)).find(f) != std::string::npos ? 100 : 0), p});
            }
            std::sort(hits.begin(), hits.end());
            for (size_t i = 0; i < hits.size() && i < 500; i++) ide.nodes.push_back({hits[i].second, hits[i].second, 0, false});
        } else {
            struct Dir { std::map<std::string, Dir> dirs; std::vector<std::string> files; };
            Dir root;
            for (auto& [p, file] : ide.files) {
                if (file.deleted) continue;
                Dir* d = &root;
                size_t start = 0;
                while (true) {
                    size_t sl = p.find('/', start);
                    if (sl == std::string::npos) break;
                    d = &d->dirs[p.substr(start, sl - start)];
                    start = sl + 1;
                }
                d->files.push_back(p);
            }
            std::function<void(const Dir&, const std::string&, int)> walk = [&](const Dir& d, const std::string& prefix, int depth) {
                for (auto& [name, sub] : d.dirs) {
                    std::string path = prefix.empty() ? name : prefix + "/" + name;
                    ide.nodes.push_back({path, name, depth, true});
                    if (ide.expanded.count(path)) walk(sub, path, depth + 1);
                }
                for (auto& p : d.files) ide.nodes.push_back({p, base_name(p), depth, false});
            };
            walk(root, "", 0);
        }
    }
    if (ide.truncated) {
        Rect tr = list.cut_bottom(30);
        r.rect(tr, T.attention_subtle);
        r.draw_text_in(FontId::Regular, 11, tr.inset(10, 0), "This repository is too big to list every file.", T.text);
    }
    auto changes = ide_changes();
    std::map<std::string, git::Change> changed(changes.begin(), changes.end());
    const float RH = 24;
    Id lid = ui.id("ide-tree");
    float off = ui::begin_scroll(ui, lid, list);
    size_t first = size_t(std::max(0.f, off / RH));
    float y = list.y - off + float(first) * RH;
    bool flat = !git::trim(ide.filter).empty();
    for (size_t i = first; i < ide.nodes.size(); i++) {
        if (y > list.bottom()) break;
        auto& n = ide.nodes[i];
        Rect row{list.x, y, list.w, RH};
        bool sel = n.path == ide.tree_sel;
        bool blue = sel && !n.dir && n.path == ide.active;
        if (sel) r.rect(row, blue ? T.row_selected_focus : T.row_selected);
        else if (ui.hovered(row)) r.rect(row, T.row_hover);
        bool rc_click = false, dbl = false;
        if (ui::row_click(ui, ui.id("ide-node", int64_t(i)), row, &dbl, &rc_click)) {
            ide.tree_sel = n.path;
            if (n.dir) {
                if (ide.expanded.count(n.path)) ide.expanded.erase(n.path); else ide.expanded.insert(n.path);
                ide.tree_ver++;
            } else {
                ide_open_file(n.path, dbl);
            }
        }
        Color fg = blue ? T.text_inverse : T.text, fg2 = blue ? T.text_inverse : T.text_muted;
        float x = row.x + 10 + float(n.depth) * 14;
        if (n.dir) {
            r.icon(ide.expanded.count(n.path) ? "chevron-down" : "chevron-right", x, row.y + 4, 16, fg2);
            r.icon("file-directory", x + 18, row.y + 4, 16, blue ? fg : T.accent_emphasis.mix(T.text_muted, 0.35f));
            r.draw_text_in(FontId::Regular, 12, {x + 40, row.y, row.right() - x - 50, row.h}, n.name, fg);
        } else {
            r.icon("file", x + 18, row.y + 4, 16, fg2);
            auto it = changed.find(n.path);
            float right_pad = it != changed.end() ? 30 : 10;
            if (flat) {
                std::string d = dir_name(n.path);
                float nw2 = r.draw_text_in(FontId::Regular, 12, {x + 40, row.y, row.right() - x - 40 - right_pad, row.h}, base_name(n.path), fg);
                if (!d.empty())
                    r.draw_text_in(FontId::Regular, 11, {x + 46 + nw2, row.y, row.right() - x - 46 - nw2 - right_pad, row.h},
                                   r.ellipsize_middle(FontId::Regular, 11, d, row.right() - x - 46 - nw2 - right_pad), fg2);
            } else {
                r.draw_text_in(FontId::Regular, 12, {x + 40, row.y, row.right() - x - 40 - right_pad, row.h},
                               r.ellipsize(FontId::Regular, 12, n.name, row.right() - x - 40 - right_pad), fg);
            }
            if (it != changed.end()) r.icon(change_icon(it->second), row.right() - 24, row.y + 4, 16, blue ? fg : change_color(T, it->second));
        }
        if (rc_click) {
            std::string p = n.path;
            ide.tree_sel = p;
            if (n.dir) {
                open_context({
                    {"New file here…", [this, p] { ide_name_d = {}; ide_name_d.path = p + "/"; dialog = Dialog::IdeNewFile; }},
                    {"Copy path", [this, p] { ui.set_clipboard(p); }},
                    {"View on GitHub", [this, p] { plat::open_url(account.html_base() + "/" + ide.owner + "/" + ide.repo + "/tree/" + ide.base + "/" + p); }},
                });
            } else {
                bool is_added = ide.files[p].added;
                open_context({
                    {"Open", [this, p] { ide_open_file(p); }},
                    {"Rename…", [this, p] { ide_name_d = {}; ide_name_d.path = p; ide_name_d.from = p; ide_name_d.rename = true; dialog = Dialog::IdeNewFile; }},
                    {"Delete", [this, p] { ide_delete(p); }},
                    {"", {}, true, true},
                    {"Copy path", [this, p] { ui.set_clipboard(p); }},
                    {"View on GitHub", [this, p] { plat::open_url(account.html_base() + "/" + ide.owner + "/" + ide.repo + "/blob/" + ide.base + "/" + p); }, !is_added},
                });
            }
        }
        y += RH;
    }
    if (ide.nodes.empty())
        r.draw_text_in(FontId::Regular, 12, {list.x, list.y + 20, list.w, 18}, flat ? "No files match." : "This repository is empty.", T.text_muted, Align::Center);
    ui::end_scroll(ui, lid, list, float(ide.nodes.size()) * RH + 4);
}

void App::draw_ide_changes(const Rect& rc) {
    auto& T = ui.theme;
    auto changes = ide_changes();
    Rect head{rc.x, rc.y, rc.w, 29};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    r.draw_text_in(FontId::Regular, 12, {head.x + 10, head.y, head.w - 20, head.h - 1},
                   changes.empty() ? "No changed files" : std::to_string(changes.size()) + " changed file" + (changes.size() == 1 ? "" : "s"), T.text);
    Rect list{rc.x, head.bottom(), rc.w, rc.h - head.h - 58};
    float off = ui::begin_scroll(ui, ui.id("ide-changes"), list);
    float y = list.y - off;
    int i = 0;
    for (auto& [p, kind] : changes) {
        Rect row{list.x, y, list.w, 29};
        bool sel = p == ide.change_sel;
        if (sel) r.rect(row, T.row_selected_focus);
        else if (ui.hovered(row)) r.rect(row, T.row_hover);
        r.line_h(row.x, row.right(), row.bottom() - 1, T.border);
        bool rc_click = false, dbl = false;
        if (ui::row_click(ui, ui.id("ide-ch", i), row, &dbl, &rc_click)) {
            ide.change_sel = p;
            ide.change_diff_ver = ~0ull;
            if (dbl && kind != git::Change::Deleted) { ide.side_tab = 0; ide_open_file(p); }
        }
        Color fg = sel ? T.text_inverse : T.text, fg2 = sel ? T.text_inverse : T.text_muted;
        std::string d = dir_name(p), n = base_name(p);
        Rect tr{row.x + 10, row.y, row.w - 46, row.h - 1};
        float nw2 = ui.text_w(FontId::Regular, 12, n);
        if (d.empty() || nw2 + 20 >= tr.w) r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, p, tr.w), fg);
        else {
            float dw = r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, d + "/", tr.w - nw2), fg2);
            r.draw_text_in(FontId::Regular, 12, {tr.x + dw, tr.y, tr.w - dw, tr.h}, n, fg);
        }
        r.icon(change_icon(kind), row.right() - 26, row.y + 6, 16, sel ? fg : change_color(T, kind));
        if (rc_click) {
            std::string pp = p;
            open_context({
                {"Discard changes", [this, pp] { ide_discard(pp); }},
                {"Open in editor", [this, pp] { ide.side_tab = 0; ide_open_file(pp); }, kind != git::Change::Deleted},
                {"Copy path", [this, pp] { ui.set_clipboard(pp); }},
            });
        }
        y += 29;
        i++;
    }
    if (changes.empty()) {
        ui::wrapped_text(ui, {list.x + 20, list.y + 20, list.w - 40, 120},
                         "Edit files in the Files tab. Everything you change here, across any number of files, goes into one pull request.",
                         FontId::Regular, 12, T.text_muted, 1.5f);
    }
    ui::end_scroll(ui, ui.id("ide-changes"), list, y + off - list.y);
    // footer like the commit box: one button for the whole set
    Rect foot{rc.x, rc.bottom() - 58, rc.w, 58};
    r.rect(foot, T.bg_subtle);
    r.line_h(foot.x, foot.right(), foot.y, T.border);
    std::string lbl = changes.empty() ? "Create pull request" : "Create pull request with " + std::to_string(changes.size()) + " file" + (changes.size() == 1 ? "" : "s");
    if (ui::button(ui, ui.id("ide-pr-side"), {foot.x + 10, foot.y + 17, foot.w - 20, 25}, lbl, ui::BtnStyle::Primary, !changes.empty())) {
        // same defaults as the toolbar button
        ide_pr_d = {};
        for (int n = 1;; n++) {
            std::string b = account.login + "-patch-" + std::to_string(n);
            if (std::find(ide.branches.begin(), ide.branches.end(), b) == ide.branches.end()) { ide_pr_d.branch = b; break; }
        }
        ide_pr_d.title = changes.size() == 1 ? std::string(changes[0].second == git::Change::Added ? "Create " : changes[0].second == git::Change::Deleted ? "Delete " : "Update ") + base_name(changes[0].first)
                                             : "Update " + std::to_string(changes.size()) + " files";
        dialog = Dialog::IdeSubmit;
    }
}

void App::draw_ide_editor(const Rect& rc) {
    auto& T = ui.theme;
    // tab strip
    Rect strip{rc.x, rc.y, rc.w, 33};
    r.rect(strip, T.bg_subtle);
    r.line_h(strip.x, strip.right(), strip.bottom() - 1, T.border);
    auto changes = ide_changes();
    std::set<std::string> changed;
    for (auto& c : changes) changed.insert(c.first);
    float x = strip.x;
    std::string to_close;
    for (size_t i = 0; i < ide.tabs.size(); i++) {
        auto& p = ide.tabs[i];
        std::string n = base_name(p);
        float w = std::min(220.f, ui.text_w(FontId::Regular, 12, n) + 58);
        Rect tr{x, strip.y, w, strip.h - 1};
        if (tr.x > strip.right()) break;
        bool act = p == ide.active;
        if (act) { r.rect(tr, T.bg); r.rect({tr.x, tr.y, tr.w, 2}, T.accent_emphasis); }
        else if (ui.hovered(tr)) r.rect(tr, T.row_hover);
        r.line_v(tr.right(), tr.y, tr.bottom(), T.border);
        r.icon("file", tr.x + 10, tr.y + 8, 16, T.text_muted);
        r.draw_text_in(FontId::Regular, 12, {tr.x + 32, tr.y, w - 56, tr.h}, r.ellipsize(FontId::Regular, 12, n, w - 56), act ? T.text : T.text_muted);
        Rect xr{tr.right() - 24, tr.y + 8, 16, 16};
        bool xh = ui.hovered(xr.inset(-2));
        if (changed.count(p) && !xh) r.rect({xr.x + 4, xr.y + 4, 8, 8}, T.text_muted, 4);
        else if (act || ui.hovered(tr)) {
            if (xh) r.rect(xr.inset(-2), T.row_selected, 4);
            r.icon("x", xr.x, xr.y, 16, T.text_muted);
        }
        if (ui.clicked(ui.id("ide-tab-x", int64_t(i)), xr.inset(-2))) to_close = p;
        else if (ui.clicked(ui.id("ide-tab", int64_t(i)), tr)) { ide.active = p; ide.tree_sel = p; }
        if (ui.hovered(tr) && ui.mouse_pressed[2]) to_close = p;
        if (ui.hovered(tr)) ui.tooltip(ui.id("ide-tab-tip", int64_t(i)), tr, p);
        x += w;
    }
    if (!to_close.empty()) ide_close_tab(to_close);
    if (ui.take_key(Key::W, plat::ModCmd) && !ide.active.empty()) ide_close_tab(ide.active);
    Rect body{rc.x, strip.bottom(), rc.w, rc.h - strip.h - 24};
    Rect status{rc.x, body.bottom(), rc.w, 24};
    r.rect(status, T.bg_subtle);
    r.line_h(status.x, status.right(), status.y, T.border);
    if (ide.active.empty() || !ide.files.count(ide.active)) {
        float cy = std::round(body.y + body.h / 2 - 110);
        illustration("empty-no-file-selected", {std::round(body.x + (body.w - 288) / 2), cy, 288, 147});
        r.draw_text_in(FontId::Bold, 12, {body.x, cy + 163, body.w, 18}, "Pick a file to start editing", T.text, Align::Center);
        r.draw_text_in(FontId::Regular, 12, {body.x, cy + 183, body.w, 18},
                       "Edit as many files as you like; they all go into one pull request.", T.text_muted, Align::Center);
        return;
    }
    auto& f = ide.files[ide.active];
    if (f.state == IdeFile::Loading || f.state == IdeFile::NotLoaded) {
        if (f.state == IdeFile::NotLoaded) ide_load(ide.active);
        ui::spinner(ui, body.x + body.w / 2, body.y + 80, 8, T.text_muted);
        return;
    }
    if (f.state == IdeFile::Failed) {
        r.draw_text_in(FontId::Regular, 12, {body.x, body.y + 60, body.w, 18}, f.error, T.text_muted, Align::Center);
        if (ui::button(ui, ui.id("ide-reload"), {body.x + body.w / 2 - 60, body.y + 90, 120, 25}, "Try again")) { f.state = IdeFile::NotLoaded; ide_load(ide.active); }
        return;
    }
    if (f.binary) {
        if (f.image && f.image->valid()) {
            float s = std::min({1.f, (body.w - 40) / float(f.image->w), (body.h - 80) / float(f.image->h)});
            float w = float(f.image->w) * s, h = float(f.image->h) * s;
            Rect ir{std::round(body.x + (body.w - w) / 2), std::round(body.y + (body.h - h) / 2 - 10), w, h};
            // checkerboard behind transparent images
            for (float yy = ir.y; yy < ir.bottom(); yy += 8)
                for (float xx = ir.x; xx < ir.right(); xx += 8)
                    if ((int((xx - ir.x) / 8) + int((yy - ir.y) / 8)) % 2)
                        r.rect({xx, yy, std::min(8.f, ir.right() - xx), std::min(8.f, ir.bottom() - yy)}, T.dark ? Color::hex(0x2f363d) : Color::hex(0xeeeeee));
            r.image(gfx::hash_bytes("ide:" + f.path + f.base_sha), *f.image, ir);
            r.draw_text_in(FontId::Regular, 12, {body.x, ir.bottom() + 10, body.w, 18},
                           f.image->format + " · " + std::to_string(f.image->w) + " × " + std::to_string(f.image->h), T.text_muted, Align::Center);
        } else {
            r.icon("file", body.x + body.w / 2 - 12, body.y + body.h / 3 - 40, 24, T.text_muted);
            r.draw_text_in(FontId::Regular, 12, {body.x, body.y + body.h / 3, body.w, 18}, "This binary file can't be edited here.", T.text_muted, Align::Center);
        }
        r.draw_text_in(FontId::Regular, 11, {status.x + 10, status.y, status.w - 20, status.h}, f.path, T.text_muted);
        return;
    }
    ui::code_editor(ui, ui.id("ide-editor"), body, f.doc);
    // status bar: position, indentation, encoding, line endings, language
    auto& d = f.doc;
    std::string pos = "Ln " + std::to_string(d.cursor.line + 1) + ", Col " + std::to_string(d.cursor.col + 1);
    if (d.has_selection()) pos += " (" + std::to_string(d.selected_text().size()) + " selected)";
    std::string right = std::string(d.use_tabs ? "Tabs" : "Spaces: " + std::to_string(d.indent)) + "     UTF-8     " +
                        (d.eol == "\r\n" ? "CRLF" : "LF") + "     " + ui::lang_name(d.lang);
    r.draw_text_in(FontId::Regular, 11, {status.x + 10, status.y, status.w / 2, status.h}, f.path, T.text_muted);
    r.draw_text_in(FontId::Regular, 11, {status.x, status.y, status.w - 10, status.h}, pos + "     " + right, T.text_muted, Align::Right);
}

// ================================================================ dialogs
static bool ide_footer(App& a, const Rect& c, const std::string& ok, bool ok_enabled, bool* cancel, const std::string& cancel_label = "Cancel") {
    auto& T = a.ui.theme;
    Rect f{c.x, c.bottom() - 65, c.w, 65};
    a.r.line_h(f.x, f.right(), f.y, T.border);
    float bw = std::max(120.f, a.ui.text_w(FontId::Regular, 12, ok) + 20);
    float cw = cancel ? std::max(120.f, a.ui.text_w(FontId::Regular, 12, cancel_label) + 20) : 0;
    bool mac = plat::os_name() == "macOS";
    float right = f.right() - 20;
    Rect okr, cr;
    if (mac || !cancel) { okr = {right - bw, f.y + 20, bw, 25}; cr = {okr.x - 5 - cw, f.y + 20, cw, 25}; }
    else { cr = {right - cw, f.y + 20, cw, 25}; okr = {cr.x - 5 - bw, f.y + 20, bw, 25}; }
    bool okc = ui::button(a.ui, a.ui.id("ide-dlg-ok"), okr, ok, ui::BtnStyle::Primary, ok_enabled);
    if (cancel) *cancel = ui::button(a.ui, a.ui.id("ide-dlg-cancel"), cr, cancel_label);
    return okc;
}

static bool valid_repo_path(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.back() == '/' || p.find("//") != std::string::npos) return false;
    size_t start = 0;
    while (true) {
        size_t sl = p.find('/', start);
        std::string seg = p.substr(start, sl == std::string::npos ? std::string::npos : sl - start);
        if (seg == "." || seg == ".." || seg == ".git") return false;
        if (sl == std::string::npos) break;
        start = sl + 1;
    }
    return p.find('\\') == std::string::npos;
}

void App::draw_ide_dialogs() {
    auto& T = ui.theme;
    bool closed = false, cancel = false;
    switch (dialog) {
        case Dialog::IdeOpen: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-ide-open"), "Edit a repository on GitHub", 560, 540, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 20;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Repository", T.text);
            y += 21;
            ui::TextFieldOpts fo;
            fo.placeholder = "owner/name or a github.com URL";
            fo.autofocus = true;
            bool enter = false;
            ui::text_field(ui, ui.id("ide-open-repo"), {x, y, w, 25}, ide_open_d.repo, fo, &enter);
            y += 35;
            ui::wrapped_text(ui, {x, y, w, 40}, "Files open straight from GitHub, nothing is cloned. Your edits to any number of files are submitted together as one pull request.",
                             FontId::Regular, 12, T.text_muted, 1.5f);
            y += 46;
            r.draw_text_in(FontId::Bold, 12, {x, y, w, 18}, "Your repositories", T.text);
            y += 24;
            Rect list{x, y, w, c.bottom() - 85 - y};
            r.rect(list, T.bg, 0, 1, T.border);
            if (gh_repos_loading && gh_repos.empty()) ui::spinner(ui, list.x + list.w / 2, list.y + 40, 8, T.text_muted);
            Rect inner = list.inset(1);
            float off = ui::begin_scroll(ui, ui.id("ide-open-list"), inner);
            float yy = inner.y - off;
            std::string f = git::to_lower(ide_open_d.repo);
            int i = 0;
            for (auto& gr : gh_repos) {
                bool match = f.empty() || git::to_lower(gr.full_name).find(f) != std::string::npos;
                if (!match) { i++; continue; }
                Rect row{inner.x, yy, inner.w, 29};
                bool sel = ide_open_d.selected == i;
                if (sel) r.rect(row, T.row_selected_focus);
                else if (ui.hovered(row)) r.rect(row, T.row_hover);
                bool dbl = false;
                if (ui::row_click(ui, ui.id("ide-open-row", i), row, &dbl)) {
                    ide_open_d.selected = i;
                    if (dbl) { dialog = Dialog::None; std::string o = gr.owner, n = gr.name; ide_guard([this, o, n] { open_ide(o, n); }); }
                }
                Color fg = sel ? T.text_inverse : T.text;
                r.icon(gr.is_private ? "lock" : gr.fork ? "repo-forked" : "repo", row.x + 10, row.y + 6, 16, fg);
                r.draw_text_in(FontId::Regular, 12, {row.x + 36, row.y, row.w - 46, row.h}, gr.full_name, fg);
                yy += 29;
                i++;
            }
            ui::end_scroll(ui, ui.id("ide-open-list"), inner, yy + off - inner.y);
            // what would open
            std::string owner, name;
            if (ide_open_d.selected >= 0 && ide_open_d.selected < int(gh_repos.size()) &&
                (f.empty() || git::to_lower(gh_repos[size_t(ide_open_d.selected)].full_name).find(f) != std::string::npos)) {
                owner = gh_repos[size_t(ide_open_d.selected)].owner;
                name = gh_repos[size_t(ide_open_d.selected)].name;
            } else {
                std::string s = git::trim(ide_open_d.repo);
                if (auto pr = gh::parse_github_remote(s, account.host())) { owner = pr->first; name = pr->second; }
                else {
                    auto sl = s.find('/');
                    if (sl != std::string::npos && sl > 0 && sl + 1 < s.size() && s.find('/', sl + 1) == std::string::npos && s.find(' ') == std::string::npos) {
                        owner = s.substr(0, sl);
                        name = s.substr(sl + 1);
                        if (name.size() > 4 && name.substr(name.size() - 4) == ".git") name.resize(name.size() - 4);
                    }
                }
            }
            bool ok = ide_footer(*this, c, owner.empty() ? "Open" : "Open " + owner + "/" + name, !owner.empty(), &cancel) || (enter && !owner.empty());
            ui::end_modal(ui);
            if (ok) { dialog = Dialog::None; ide_guard([this, owner, name] { open_ide(owner, name); }); }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::IdeNewFile: {
            bool ren = ide_name_d.rename;
            Rect c = ui::begin_modal(ui, ui.id("dlg-ide-name"), ren ? "Rename file" : "New file", 480, 230, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 20;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Path in " + ide.owner + "/" + ide.repo, T.text);
            y += 21;
            ui::TextFieldOpts fo;
            fo.placeholder = "src/new_file.cpp";
            fo.autofocus = true;
            bool enter = false;
            ui::text_field(ui, ui.id("ide-name-path"), {x, y, w, 25}, ide_name_d.path, fo, &enter);
            y += 33;
            std::string p = git::trim(ide_name_d.path);
            bool exists = ide.files.count(p) && !ide.files[p].deleted && p != ide_name_d.from;
            bool valid = valid_repo_path(p) && !exists && p != ide_name_d.from;
            if (exists) r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "A file with that name already exists.", T.danger);
            else if (!p.empty() && !valid_repo_path(p)) r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "That isn't a valid path.", T.danger);
            else r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Use slashes to put it in a folder; folders are created for you.", T.text_muted);
            bool ok = ide_footer(*this, c, ren ? "Rename" : "Create file", valid, &cancel) || (enter && valid);
            ui::end_modal(ui);
            if (ok) {
                dialog = Dialog::None;
                if (ren) ide_rename(ide_name_d.from, p);
                else ide_new_file(p);
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::IdeConfirmDiscard: {
            auto n = ide_changes().size();
            Rect c = ui::begin_modal(ui, ui.id("dlg-ide-discard"), "Discard your edits?", 480, 200, &closed);
            ui::wrapped_text(ui, {c.x + 20, c.y + 20, c.w - 40, 60},
                             "You have changes to " + std::to_string(n) + " file" + (n == 1 ? "" : "s") + " that haven't been submitted as a pull request. They'll be lost.",
                             FontId::Regular, 12, T.text, 1.5f);
            Rect f{c.x, c.bottom() - 65, c.w, 65};
            r.line_h(f.x, f.right(), f.y, T.border);
            bool disc = ui::button(ui, ui.id("ide-disc-ok"), {f.right() - 140, f.y + 20, 120, 25}, "Discard edits", ui::BtnStyle::Danger);
            bool keep = ui::button(ui, ui.id("ide-disc-keep"), {f.right() - 265, f.y + 20, 120, 25}, "Keep editing");
            ui::end_modal(ui);
            if (disc) { dialog = Dialog::None; auto fn = std::move(ide_after_discard); ide_after_discard = {}; if (fn) fn(); }
            if (keep || closed) { dialog = Dialog::None; ide_after_discard = {}; }
            break;
        }
        case Dialog::IdeSubmit: {
            auto changes = ide_changes();
            float list_h = std::min(float(changes.size()), 5.f) * 22 + (changes.size() > 5 ? 20 : 0);
            float err_h = ide_pr_d.error.empty() ? 0 : ui::wrapped_text(ui, {0, 0, 476, 200}, ide_pr_d.error, FontId::Regular, 12, T.text, 1.4f, true) + 26;
            float h = 445 + list_h + (ide.can_push ? 0 : 56) + err_h;
            Rect c = ui::begin_modal(ui, ui.id("dlg-ide-pr"), "Create a pull request", 560, h, ide_pr_d.submitting ? nullptr : &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 20;
            if (!ide.can_push) {
                Rect b{x, y, w, 46};
                r.rect(b, T.accent_subtle, 6, 1, T.dark ? Color::hex(0x044289) : Color::hex(0xc8e1ff));
                r.icon("repo-forked", b.x + 10, b.y + 15, 16, T.text);
                ui::wrapped_text(ui, {b.x + 34, b.y + 6, b.w - 44, 40}, "You can't push to " + ide.owner + "/" + ide.repo + ", so the branch goes to your fork (@" +
                                 account.login + "/" + ide.repo + ") and the pull request is opened from there.", FontId::Regular, 12, T.text, 1.4f);
                y += 56;
            }
            r.draw_text_in(FontId::Bold, 12, {x, y, w, 18}, std::to_string(changes.size()) + " changed file" + (changes.size() == 1 ? "" : "s") + " → " + ide.base, T.text);
            y += 24;
            for (size_t i = 0; i < changes.size() && i < 5; i++) {
                r.icon(change_icon(changes[i].second), x, y + 3, 16, change_color(T, changes[i].second));
                r.draw_text_in(FontId::Regular, 12, {x + 22, y, w - 22, 22}, r.ellipsize_middle(FontId::Regular, 12, changes[i].first, w - 22), T.text);
                y += 22;
            }
            if (changes.size() > 5) { r.draw_text_in(FontId::Regular, 12, {x + 22, y, w, 20}, "and " + std::to_string(changes.size() - 5) + " more", T.text_muted); y += 20; }
            y += 12;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Title", T.text);
            y += 21;
            ui::TextFieldOpts to;
            to.placeholder = "Title (required)";
            to.autofocus = true;
            ui::text_field(ui, ui.id("ide-pr-title"), {x, y, w, 25}, ide_pr_d.title, to);
            y += 35;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Description", T.text);
            y += 21;
            ui::TextFieldOpts bo;
            bo.multiline = true;
            bo.placeholder = "Explain what you changed and why (optional)";
            ui::text_field(ui, ui.id("ide-pr-body"), {x, y, w, 90}, ide_pr_d.body, bo);
            y += 100;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "New branch", T.text);
            y += 21;
            ui::TextFieldOpts bro;
            ui::text_field(ui, ui.id("ide-pr-branch"), {x, y, w, 25}, ide_pr_d.branch, bro);
            y += 33;
            ui::checkbox_label(ui, ui.id("ide-pr-draft"), {x, y, w, 22}, ide_pr_d.draft, "Open as a draft pull request");
            y += 30;
            if (!ide_pr_d.error.empty()) {
                float eh = ui::wrapped_text(ui, {0, 0, w - 44, 200}, ide_pr_d.error, FontId::Regular, 12, T.text, 1.4f, true) + 16;
                Rect eb{x, y, w, eh};
                r.rect(eb, T.danger_subtle, 6);
                r.icon("alert", eb.x + 10, eb.y + 9, 16, T.danger);
                ui::wrapped_text(ui, {eb.x + 34, eb.y + 8, eb.w - 44, eh}, ide_pr_d.error, FontId::Regular, 12, T.text, 1.4f);
            }
            std::string br = git::trim(ide_pr_d.branch);
            bool branch_ok = !br.empty() && git::sanitize_branch_name(br) == br &&
                             std::find(ide.branches.begin(), ide.branches.end(), br) == ide.branches.end();
            bool can = !git::trim(ide_pr_d.title).empty() && branch_ok && !ide_pr_d.submitting && !changes.empty();
            if (ide_pr_d.submitting) {
                ui::spinner(ui, c.x + 30, c.bottom() - 32, 6, T.text_muted);
                r.draw_text_in(FontId::Regular, 12, {c.x + 44, c.bottom() - 42, c.w - 250, 20}, ide_pr_d.progress, T.text_muted);
            } else if (!branch_ok && !br.empty()) {
                r.draw_text_in(FontId::Regular, 12, {c.x + 20, c.bottom() - 42, c.w - 250, 20}, "Pick a new, valid branch name.", T.danger);
            }
            bool ok = ide_footer(*this, c, "Create pull request", can, ide_pr_d.submitting ? nullptr : &cancel);
            ui::end_modal(ui);
            if (ok) ide_submit();
            if ((closed || cancel) && !ide_pr_d.submitting) dialog = Dialog::None;
            break;
        }
        default: break;
    }
}

}  // namespace app
