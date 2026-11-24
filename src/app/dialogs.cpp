// SPDX-License-Identifier: MIT
// Modal dialogs.
#include <algorithm>

#include "../net/http.hpp"
#include "app.hpp"
#include "../git/lfs.hpp"
#include "../git/process.hpp"

namespace app {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using plat::Key;
namespace fs = std::filesystem;

static const char* kVersion = "0.2.0";

static std::string human_size(uint64_t b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(b) / 1024);
    else std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / 1024 / 1024);
    return buf;
}

// Field with a label above it; returns the rect below.
static float field_label(App& a, float x, float y, float w, const std::string& text) {
    a.r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, text, a.ui.theme.text);
    return y + 21;
}

static bool footer(App& a, const Rect& content, const std::string& ok, bool ok_enabled, bool* cancel,
                   ui::BtnStyle style = ui::BtnStyle::Primary, const std::string& cancel_label = "Cancel") {
    auto& T = a.ui.theme;
    // .dialog-footer: padding 20, 120px-min buttons 5px apart. OkCancelButtonGroup puts
    // OK last on macOS and first everywhere else.
    Rect f{content.x, content.bottom() - 65, content.w, 65};
    a.r.line_h(f.x, f.right(), f.y, T.border);
    float bw = std::max(120.f, a.ui.text_w(FontId::Regular, 12, ok) + 20);
    float cw = cancel ? std::max(120.f, a.ui.text_w(FontId::Regular, 12, cancel_label) + 20) : 0;
    bool mac = plat::os_name() == "macOS";
    float right = f.right() - 20;
    Rect okr, cr;
    if (mac || !cancel) { okr = {right - bw, f.y + 20, bw, 25}; cr = {okr.x - 5 - cw, f.y + 20, cw, 25}; }
    else { cr = {right - cw, f.y + 20, cw, 25}; okr = {cr.x - 5 - bw, f.y + 20, bw, 25}; }
    bool okc = ui::button(a.ui, a.ui.id("dlg-ok"), okr, ok, style, ok_enabled);
    if (cancel) *cancel = ui::button(a.ui, a.ui.id("dlg-cancel"), cr, cancel_label);
    if (ok_enabled && a.ui.key_pressed(Key::Enter, 0) && a.ui.focus == 0) { a.ui.take_key(Key::Enter, 0); okc = true; }
    return okc;
}

static std::string repo_name_from_url(const std::string& url) {
    std::string u = git::trim(url);
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() > 4 && u.substr(u.size() - 4) == ".git") u.resize(u.size() - 4);
    auto slash = u.find_last_of("/:");
    return slash == std::string::npos ? u : u.substr(slash + 1);
}

static void path_picker(App& a, float x, float y, float w, std::string& path, ui::Id id, const std::string& title) {
    float bw = 100;
    ui::text_field(a.ui, id, {x, y, w - bw - 8, 25}, path, {});
    if (ui::button(a.ui, id ^ 0xb7, {x + w - bw, y, bw, 25}, "Choose…")) {
        std::string start = path.empty() ? git::path_to_git(git::home_dir()) : path;
        if (auto p = a.win.pick_folder(title, start)) path = *p;
    }
}

void App::draw_dialogs() {
    auto& T = ui.theme;
    bool closed = false, cancel = false;
    switch (dialog) {
        case Dialog::None: return;

        // ------------------------------------------------------------ clone
        case Dialog::Clone: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-clone"), "Clone a repository", 600, 560, &closed);
            float x = c.x + 20, w = c.w - 40;
            int nt = ui::tabs(ui, ui.id("clone-tabs"), {c.x, c.y, c.w, 40}, {"GitHub.com", "URL"}, clone_d.tab);
            if (nt != clone_d.tab) { clone_d.tab = nt; if (nt == 0) load_gh_repos(); }
            float y = c.y + 56;
            if (clone_d.tab == 0) {
                if (!account.valid()) {
                    ui::wrapped_text(ui, {x, y, w, 60}, "Sign in to GitHub to see your repositories here, or use the URL tab.", FontId::Regular, 12, T.text_muted);
                    if (ui::button(ui, ui.id("clone-signin"), {x, y + 40, 120, 25}, "Sign in", ui::BtnStyle::Primary)) dialog = Dialog::SignIn;
                    y += 300;
                } else {
                    ui::TextFieldOpts fo;
                    fo.placeholder = "Filter your repositories";
                    fo.icon = "search";
                    ui::text_field(ui, ui.id("clone-filter"), {x, y, w - 40, 25}, clone_d.filter, fo);
                    if (ui::icon_button(ui, ui.id("clone-reload"), {x + w - 32, y, 32, 25}, "sync", "Refresh")) load_gh_repos(true);
                    y += 35;
                    Rect list{x, y, w, 250};
                    r.rect(list, T.bg, 6, 1, T.border);
                    Rect inner = list.inset(1);
                    if (gh_repos_loading && gh_repos.empty()) ui::spinner(ui, inner.x + inner.w / 2, inner.y + 40, 8, T.text_muted);
                    float off = ui::begin_scroll(ui, ui.id("clone-list"), inner);
                    float yy = inner.y - off;
                    std::string f = git::to_lower(clone_d.filter);
                    std::string last_owner;
                    for (int i = 0; i < int(gh_repos.size()); i++) {
                        auto& gr = gh_repos[size_t(i)];
                        if (!f.empty() && git::to_lower(gr.full_name).find(f) == std::string::npos) continue;
                        Rect row{inner.x, yy, inner.w, 30};
                        if (row.bottom() >= inner.y && row.y <= inner.bottom()) {
                            bool dbl = false;
                            if (ui::selectable(ui, ui.id("clone-row", i), row, clone_d.selected == i, &dbl)) {
                                clone_d.selected = i;
                                clone_d.url = gr.clone_url;
                                if (!clone_d.path_edited) clone_d.path = settings.clone_dir + "/" + gr.name;
                            }
                            r.icon(gr.is_private ? "lock" : gr.fork ? "repo-forked" : "repo", row.x + 12, row.y + 8, 16, T.text_muted);
                            r.draw_text_in(FontId::Regular, 12, {row.x + 38, row.y, row.w - 50, row.h}, gr.full_name, T.text);
                        }
                        yy += 30;
                    }
                    ui::end_scroll(ui, ui.id("clone-list"), inner, yy + off - inner.y);
                    y += 262;
                }
            } else {
                y = field_label(*this, x, y, w, "Repository URL or GitHub username and repository");
                ui::TextFieldOpts fo;
                fo.placeholder = "URL or username/repository";
                fo.autofocus = true;
                if (ui::text_field(ui, ui.id("clone-url"), {x, y, w, 25}, clone_d.url, fo) && !clone_d.path_edited) {
                    auto n = repo_name_from_url(clone_d.url);
                    clone_d.path = n.empty() ? settings.clone_dir : settings.clone_dir + "/" + n;
                }
                y += 40;
                r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, "e.g. https://github.com/owner/repo or owner/repo", T.text_muted);
                y += 220;
            }
            y = field_label(*this, x, y, w, "Local path");
            std::string before = clone_d.path;
            path_picker(*this, x, y, w, clone_d.path, ui.id("clone-path"), "Choose where to clone");
            if (clone_d.path != before) clone_d.path_edited = true;
            y += 40;
            bool url_ok = !git::trim(clone_d.url).empty();
            if (fs::exists(git::git_to_path(clone_d.path)) && !fs::is_empty(git::git_to_path(clone_d.path)))
                r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, "This folder already exists and isn't empty.", T.attention);
            bool ok = footer(*this, c, "Clone", url_ok && !clone_d.path.empty() && !jobs.busy(), &cancel);
            ui::end_modal(ui);
            if (ok) {
                fs::create_directories(git::git_to_path(clone_d.path).parent_path());
                clone_repo(clone_d.url, clone_d.path);
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ create
        case Dialog::Create: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-create"), "Create a new repository", 560, 520, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Name");
            ui::TextFieldOpts fo;
            fo.placeholder = "repository name";
            fo.autofocus = true;
            ui::text_field(ui, ui.id("create-name"), {x, y, w, 25}, create_d.name, fo);
            y += 40;
            std::string sanitized = git::sanitize_branch_name(create_d.name);
            for (auto& ch : sanitized) if (ch == '/') ch = '-';
            if (!create_d.name.empty() && sanitized != create_d.name)
                r.draw_text_in(FontId::Regular, 12, {x, y - 4, w, 18}, "Will be created as " + sanitized, T.attention);
            y += 14;
            y = field_label(*this, x, y, w, "Description");
            ui::text_field(ui, ui.id("create-desc"), {x, y, w, 25}, create_d.description, {});
            y += 40;
            y = field_label(*this, x, y, w, "Local path");
            path_picker(*this, x, y, w, create_d.path, ui.id("create-path"), "Choose a folder");
            y += 52;
            ui::checkbox_label(ui, ui.id("create-readme"), {x, y, w, 24}, create_d.readme, "Initialize this repository with a README");
            y += 32;
            ui::checkbox_label(ui, ui.id("create-gi"), {x, y, w, 24}, create_d.gitignore, "Add a .gitignore");
            y += 32;
            ui::checkbox_label(ui, ui.id("create-mit"), {x, y, w, 24}, create_d.mit, "Add an MIT License");
            bool ok = footer(*this, c, "Create repository", !sanitized.empty() && !create_d.path.empty(), &cancel);
            ui::end_modal(ui);
            if (ok) { create_d.name = sanitized; create_repo(); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ add existing
        case Dialog::AddExisting: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-add"), "Add local repository", 560, 260, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Local path");
            path_picker(*this, x, y, w, add_d.path, ui.id("add-path"), "Choose a repository folder");
            y += 35;
            bool valid = !add_d.path.empty() && git::Repository::discover(git::git_to_path(add_d.path)).has_value();
            if (!add_d.path.empty() && !valid) {
                r.icon("alert", x, y + 2, 16, T.attention);
                r.draw_text_in(FontId::Regular, 12, {x + 22, y, w - 22, 20}, "This directory does not appear to be a Git repository.", T.attention);
                if (ui::button(ui, ui.id("add-create"), {x + 22, y + 24, 230, 24}, "Create a repository here instead?", ui::BtnStyle::Link)) {
                    create_d = {};
                    auto p = fs::path(git::git_to_path(add_d.path));
                    create_d.name = git::path_to_git(p.filename());
                    create_d.path = git::path_to_git(p.parent_path());
                    dialog = Dialog::Create;
                }
            }
            bool ok = footer(*this, c, "Add repository", valid, &cancel);
            ui::end_modal(ui);
            if (ok) { dialog = Dialog::None; add_repo_path(add_d.path, true); }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ branches
        case Dialog::NewBranch: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-newbranch"), "Create a branch", 520, branch_d.from_commit ? 260 : 330, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Name");
            ui::TextFieldOpts fo;
            fo.autofocus = true;
            fo.placeholder = "my-new-feature";
            bool enter = false;
            ui::text_field(ui, ui.id("nb-name"), {x, y, w, 25}, branch_d.name, fo, &enter);
            y += 40;
            std::string san = git::sanitize_branch_name(branch_d.name);
            bool exists = false;
            for (auto& b : rs->branches) if (!b.remote && b.name == san) exists = true;
            if (!branch_d.name.empty() && san != branch_d.name) r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Will be created as " + san, T.attention);
            if (exists) r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "A branch named " + san + " already exists.", T.danger);
            y += 28;
            std::string defb = rs->default_branch.empty() ? "main" : rs->default_branch;
            if (branch_d.from_commit) {
                r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, "Your new branch will be based on commit " + branch_d.from_commit->short_hex() + ".", T.text_muted);
            } else {
                r.draw_text_in(FontId::Bold, 12, {x, y, w, 20}, "Create branch based on…", T.text);
                y += 28;
                if (ui::radio(ui, ui.id("nb-cur"), {x, y, w, 24}, branch_d.from == 0, (rs->branch.empty() ? "HEAD" : rs->branch) + " (current branch)")) branch_d.from = 0;
                y += 30;
                if (defb != rs->branch && ui::radio(ui, ui.id("nb-def"), {x, y, w, 24}, branch_d.from == 1, defb + " (default branch)")) branch_d.from = 1;
            }
            bool ok = footer(*this, c, "Create branch", !san.empty() && !exists, &cancel) || (enter && !san.empty() && !exists);
            ui::end_modal(ui);
            if (ok) {
                std::optional<ObjectId> from = branch_d.from_commit;
                if (!from && branch_d.from == 1) {
                    for (auto& b : rs->branches) if (b.name == defb || b.name == remote_name() + "/" + defb) { from = b.tip; if (!b.remote) break; }
                }
                create_branch(san, from);
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::RenameBranch: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-rename"), "Rename branch", 480, 210, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Name");
            ui::TextFieldOpts fo;
            fo.autofocus = true;
            ui::text_field(ui, ui.id("rn-name"), {x, y, w, 25}, rename_d.to, fo);
            std::string san = git::sanitize_branch_name(rename_d.to);
            bool ok = footer(*this, c, "Rename " + rename_d.from, !san.empty() && san != rename_d.from, &cancel);
            ui::end_modal(ui);
            if (ok) { rename_branch(rename_d.from, san); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::DeleteBranch: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-delete"), "Delete branch", 480, 240, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::wrapped_text(ui, {x, y, w, 60}, "Delete branch \"" + delete_d.name + "\"? This action cannot be undone.", FontId::Regular, 12, T.text);
            y += 50;
            bool has_remote = false;
            for (auto& b : rs->branches) if (b.name == delete_d.name && !b.upstream.empty()) has_remote = true;
            if (has_remote) ui::checkbox_label(ui, ui.id("del-remote"), {x, y, w, 24}, delete_d.remote, "Also delete this branch on the remote");
            bool ok = footer(*this, c, "Delete", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { delete_branch(delete_d.name, delete_d.remote); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::Merge: {
            const char* titles[] = {"Merge into ", "Squash and merge into ", "Rebase "};
            Rect c = ui::begin_modal(ui, ui.id("dlg-merge"), std::string(titles[merge_d.mode]) + rs->branch, 540, 560, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 16;
            ui::TextFieldOpts fo;
            fo.placeholder = "Filter";
            fo.icon = "search";
            fo.autofocus = true;
            ui::text_field(ui, ui.id("merge-filter"), {x, y, w, 25}, merge_d.filter, fo);
            y += 35;
            Rect list{x, y, w, c.bottom() - 120 - y};
            r.rect(list, T.bg, 6, 1, T.border);
            Rect inner = list.inset(1);
            float off = ui::begin_scroll(ui, ui.id("merge-list"), inner);
            float yy = inner.y - off;
            int i = 0;
            std::string f = git::to_lower(merge_d.filter);
            for (auto& b : rs->branches) {
                if (b.current) continue;
                if (!f.empty() && git::to_lower(b.name).find(f) == std::string::npos) continue;
                Rect row{inner.x, yy, inner.w, 30};
                if (ui::selectable(ui, ui.id("mb", i++), row, merge_d.selected == b.full)) merge_d.selected = b.full;
                r.icon("git-branch", row.x + 12, row.y + 8, 16, T.text_muted);
                r.draw_text_in(FontId::Regular, 12, {row.x + 38, row.y, row.w - 50, row.h}, b.name, T.text);
                yy += 30;
            }
            ui::end_scroll(ui, ui.id("merge-list"), inner, yy + off - inner.y);
            // live preview (computed on the worker)
            if (!merge_d.selected.empty() && merge_d.previewed != merge_d.selected + std::to_string(merge_d.mode) && !merge_d.preview_loading) {
                merge_d.preview_loading = true;
                merge_d.previewed = merge_d.selected + std::to_string(merge_d.mode);
                auto ref = merge_d.selected;
                auto path = rs->path;
                jobs.run("Preview", [this, ref, path] {
                    git::MergePreview p;
                    int mine = 0;
                    try {
                        auto repo = git::Repository::open(git::git_to_path(path));
                        p = repo.merge_preview(ref);
                        if (auto h = repo.head(); h)
                            if (auto o = repo.refs().resolve(ref))
                                if (auto mb = repo.merge_base(*h, *o)) mine = int(repo.commits_between(*mb, *h).size());
                    } catch (...) {}
                    jobs.post([this, p, mine] { merge_d.preview = p; merge_d.rebase_commits = mine; merge_d.preview_loading = false; });
                });
            }
            Rect pv{x, c.bottom() - 110, w, 44};
            if (!merge_d.selected.empty()) {
                auto& p = merge_d.preview;
                std::string other = git::shorten_ref(merge_d.selected), cur = rs->branch;
                std::string msg;
                const char* ic = "check";
                Color col = T.success;
                if (merge_d.preview_loading) { msg = "Checking for conflicts…"; ic = "sync"; col = T.text_muted; }
                else if (merge_d.mode == 2) {
                    if (p.kind == git::MergePreview::UpToDate && merge_d.rebase_commits == 0) msg = "This branch is up to date with " + other + ".";
                    else msg = "This will update " + cur + " by applying its " + std::to_string(merge_d.rebase_commits) + " commit" +
                               (merge_d.rebase_commits == 1 ? "" : "s") + " on top of " + other + ".";
                    if (p.kind == git::MergePreview::Conflicts) { ic = "alert"; col = T.attention; msg += " Conflicts are likely."; }
                } else switch (p.kind) {
                    case git::MergePreview::UpToDate: msg = "This branch is up to date with " + other + "."; break;
                    case git::MergePreview::FastForward:
                    case git::MergePreview::Clean:
                        msg = std::string(merge_d.mode == 1 ? "This will squash " : "This will merge ") + std::to_string(p.commits) + " commit" +
                              (p.commits == 1 ? "" : "s") + " from " + other + " into " + cur + ".";
                        break;
                    case git::MergePreview::Conflicts:
                        ic = "alert"; col = T.attention;
                        msg = "There will be " + std::to_string(p.conflicts) + " conflicted file" + (p.conflicts == 1 ? "" : "s") + " when merging " + other + " into " + cur + ".";
                        break;
                    case git::MergePreview::Unrelated: ic = "alert"; col = T.danger; msg = "These branches have unrelated histories."; break;
                }
                r.icon(ic, pv.x, pv.y + 7, 16, col);
                ui::wrapped_text(ui, {pv.x + 24, pv.y + 6, pv.w - 24, 40}, msg, FontId::Regular, 12, T.text);
            }
            if (has_changes()) r.draw_text_in(FontId::Regular, 12, {x, c.bottom() - 46, w - 300, 25}, "Commit or stash your changes first.", T.attention);
            // footer with mode switcher
            Rect ft{c.x, c.bottom() - 65, c.w, 60};
            r.line_h(ft.x, ft.right(), ft.y, T.border);
            std::string other = merge_d.selected.empty() ? "" : git::shorten_ref(merge_d.selected);
            const char* verbs[] = {"Merge", "Squash and merge", "Rebase"};
            std::string label = other.empty() ? verbs[merge_d.mode] : merge_d.mode == 2 ? "Rebase " + rs->branch + " onto " + other
                                                                                      : std::string(verbs[merge_d.mode]) + " " + other + " into " + rs->branch;
            float bw = std::min(c.w - 80, ui.text_w(FontId::Regular, 12, label) + 40);
            Rect okb{ft.right() - bw - 58, ft.y + 20, bw, 25};
            Rect drop{okb.right() + 2, ft.y + 20, 30, 25};
            bool can = !merge_d.selected.empty() && !has_changes() && !merge_d.preview_loading;
            bool ok = ui::button(ui, ui.id("merge-go"), okb, label, ui::BtnStyle::Primary, can);
            auto mpid = ui.id("merge-mode");
            if (ui::button(ui, ui.id("merge-mode-btn"), drop, "", ui::BtnStyle::Primary, true, "chevron-down")) ui.open_popup(mpid);
            Rect mm{drop.right() - 260, drop.y - 110, 260, 104};
            if (ui::begin_popup(ui, mpid, mm)) {
                for (int k = 0; k < 3; k++)
                    if (ui::menu_item(ui, ui.id("mm", k), {mm.x, mm.y + 4 + float(k) * 32, mm.w, 25}, verbs[k], "", true, nullptr, merge_d.mode == k))
                        merge_d.mode = k;
                ui::end_popup(ui);
            }
            ui::end_modal(ui);
            if (ok) {
                if (merge_d.mode == 0) merge_into_current(merge_d.selected);
                else if (merge_d.mode == 1) squash_merge(merge_d.selected);
                else rebase_onto(merge_d.selected);
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ publish
        case Dialog::Publish: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-publish"), "Publish repository", 540, 440, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Name");
            ui::text_field(ui, ui.id("pub-name"), {x, y, w, 25}, publish_d.name, {});
            y += 40;
            y = field_label(*this, x, y, w, "Description");
            ui::text_field(ui, ui.id("pub-desc"), {x, y, w, 25}, publish_d.description, {});
            y += 40;
            ui::checkbox_label(ui, ui.id("pub-priv"), {x, y, w, 24}, publish_d.priv, "Keep this code private");
            y += 36;
            y = field_label(*this, x, y, w, "Organization");
            if (ui::radio(ui, ui.id("pub-none"), {x, y, w / 2, 24}, publish_d.org.empty(), "None (" + account.login + ")")) publish_d.org.clear();
            y += 28;
            int i = 0;
            for (auto& o : publish_d.orgs) {
                if (ui::radio(ui, ui.id("pub-org", i++), {x, y, w / 2, 24}, publish_d.org == o, o)) publish_d.org = o;
                y += 28;
                if (y > c.bottom() - 90) break;
            }
            bool ok = footer(*this, c, "Publish repository", !git::trim(publish_d.name).empty() && !jobs.busy(), &cancel);
            ui::end_modal(ui);
            if (ok) { publish_repo(); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ settings
        case Dialog::Settings: {
            if (!settings_d.loaded) {
                auto cfg = git::Config::load(git::global_config_path());
                settings_d.name = cfg.get("user.name").value_or("");
                settings_d.email = cfg.get("user.email").value_or("");
                settings_d.default_branch = cfg.get("init.defaultbranch").value_or(settings.default_branch);
                settings_d.sign = cfg.get_bool("commit.gpgsign", false);
                settings_d.signing_key = cfg.get("user.signingkey").value_or("");
                auto fmt = git::to_lower(cfg.get("gpg.format").value_or("openpgp"));
                settings_d.sign_format = fmt == "ssh" ? 1 : fmt == "x509" ? 2 : 0;
                settings_d.loaded = true;
            }
            Rect c = ui::begin_modal(ui, ui.id("dlg-settings"), plat::os_name() == "Windows" ? "Options" : "Settings", 760, 520, &closed);
            // .tab-bar.vertical: 150px items, padding 10, margin 3px 20px, blue when selected
            Rect nav{c.x, c.y, 190, c.h - 65};
            const char* tabsl[] = {"Accounts", "Integrations", "Git", "Appearance", "Advanced"};
            const char* icons[] = {"person", "code", "git-commit", "eye", "gear"};
            for (int i = 0; i < 5; i++) {
                Rect row{nav.x + 20, nav.y + 13 + float(i) * 44, 150, 38};
                bool sel = settings_d.tab == i;
                if (sel) r.rect(row, T.accent_emphasis, 6);
                else if (ui.hovered(row)) r.rect(row, T.row_hover, 6);
                if (ui.clicked(ui.id("set-tab", i), row)) settings_d.tab = i;
                Color fg = sel ? T.text_inverse : T.text;
                r.icon(icons[i], row.x + 10, row.y + 11, 16, fg);
                r.draw_text_in(FontId::Regular, 12, {row.x + 36, row.y, row.w - 40, row.h}, tabsl[i], fg);
            }
            float x = nav.right() + 20, w = c.right() - x - 20, y = c.y + 20;
            switch (settings_d.tab) {
                case 0:
                    r.draw_text_in(FontId::Bold, 14, {x, y, w, 24}, "GitHub.com", T.text);
                    y += 36;
                    if (account.valid()) {
                        draw_avatar({x, y, 40, 40}, account.name, account.email);
                        r.draw_text_in(FontId::Bold, 14, {x + 52, y, w - 160, 20}, account.name, T.text);
                        r.draw_text_in(FontId::Regular, 12, {x + 52, y + 20, w - 160, 20}, "@" + account.login, T.text_muted);
                        if (ui::button(ui, ui.id("set-signout"), {x + w - 100, y + 4, 100, 25}, "Sign out")) sign_out();
                    } else {
                        ui::wrapped_text(ui, {x, y, w, 40}, "Sign in to your GitHub.com account to access your repositories.", FontId::Regular, 12, T.text_muted);
                        if (ui::button(ui, ui.id("set-signin"), {x, y + 40, 140, 25}, "Sign in", ui::BtnStyle::Primary)) { ghe_d.signin_tab = 0; dialog = Dialog::SignIn; }
                    }
                    y += 100;
                    r.line_h(x, x + w, y, T.border);
                    y += 16;
                    r.draw_text_in(FontId::Bold, 14, {x, y, w, 24}, "GitHub Enterprise", T.text);
                    y += 36;
                    if (enterprise.valid()) {
                        draw_avatar({x, y, 40, 40}, enterprise.name, enterprise.email);
                        r.draw_text_in(FontId::Bold, 14, {x + 52, y, w - 160, 20}, enterprise.name, T.text);
                        r.draw_text_in(FontId::Regular, 12, {x + 52, y + 20, w - 160, 20}, "@" + enterprise.login + " on " + enterprise.host(), T.text_muted);
                        if (ui::button(ui, ui.id("set-ghe-out"), {x + w - 100, y + 4, 100, 25}, "Sign out")) {
                            plat::secret_delete("ghe:" + enterprise.host() + ":" + enterprise.login);
                            enterprise = gh::Account{};
                            save_state();
                        }
                    } else {
                        ui::wrapped_text(ui, {x, y, w, 40}, "If you have a GitHub Enterprise account at work, sign in to it here.", FontId::Regular, 12, T.text_muted);
                        if (ui::button(ui, ui.id("set-ghe-in"), {x, y + 40, 140, 25}, "Sign in")) { ghe_d.signin_tab = 1; dialog = Dialog::SignIn; }
                    }
                    break;
                case 1:
                    y = field_label(*this, x, y, w, "External editor command");
                    {
                        ui::TextFieldOpts o;
                        o.placeholder = plat::os_name() == "macOS" ? "Automatic (VS Code, Zed or default app)" : "Automatic (code)";
                        ui::text_field(ui, ui.id("set-editor"), {x, y, w, 25}, settings.editor_cmd, o);
                    }
                    y += 40;
                    r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "e.g. code, zed, subl, \"open -a Xcode\"", T.text_muted);
                    y += 34;
                    y = field_label(*this, x, y, w, "Shell / terminal command");
                    {
                        ui::TextFieldOpts o;
                        o.placeholder = plat::os_name() == "macOS" ? "Automatic (Terminal)" : plat::os_name() == "Windows" ? "Automatic (Windows Terminal)" : "Automatic";
                        ui::text_field(ui, ui.id("set-shell"), {x, y, w, 25}, settings.shell_cmd, o);
                    }
                    y += 40;
                    r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "The repository path is appended, e.g. \"open -a iTerm\"", T.text_muted);
                    break;
                case 2:
                    y = field_label(*this, x, y, w, "Name");
                    ui::text_field(ui, ui.id("set-name"), {x, y, w, 25}, settings_d.name, {});
                    y += 40;
                    y = field_label(*this, x, y, w, "Email");
                    ui::text_field(ui, ui.id("set-email"), {x, y, w, 25}, settings_d.email, {});
                    y += 40;
                    if (account.valid() && ui::button(ui, ui.id("set-usegh"), {x, y, 240, 24}, "Use my GitHub name and email", ui::BtnStyle::Link)) {
                        settings_d.name = account.name.empty() ? account.login : account.name;
                        settings_d.email = account.email;
                    }
                    y += 36;
                    y = field_label(*this, x, y, w, "Default branch name for new repositories");
                    ui::text_field(ui, ui.id("set-defbranch"), {x, y, w / 2, 25}, settings_d.default_branch, {});
                    y += 40;
                    // commit signing (commit.gpgsign, gpg.format, user.signingkey)
                    ui::checkbox_label(ui, ui.id("set-sign"), {x, y, w, 22}, settings_d.sign, "Sign commits");
                    y += 28;
                    if (settings_d.sign) {
                        const char* fmts[] = {"GPG", "SSH", "X.509"};
                        for (int i = 0; i < 3; i++)
                            if (ui::radio(ui, ui.id("set-signfmt", i), {x + float(i) * 90, y, 86, 22}, settings_d.sign_format == i, fmts[i])) settings_d.sign_format = i;
                        y += 28;
                        ui::TextFieldOpts ko;
                        ko.placeholder = settings_d.sign_format == 1 ? "~/.ssh/id_ed25519.pub or key::ssh-ed25519 AAAA…"
                                         : "Key ID (leave empty to use your email)";
                        ui::text_field(ui, ui.id("set-signkey"), {x, y, w, 25}, settings_d.signing_key, ko);
                        y += 31;
                        std::string prog = settings_d.sign_format == 1 ? "ssh-keygen" : settings_d.sign_format == 2 ? "gpgsm" : "gpg";
                        bool found = !git::find_program(prog).empty();
                        r.draw_text_in(FontId::Regular, 11, {x, y, w, 16},
                                       found ? "Uses " + prog + " from your PATH." : prog + " wasn't found on your PATH. Install it to sign commits.",
                                       found ? T.text_muted : T.danger);
                    }
                    break;
                case 3: {
                    r.draw_text_in(FontId::Bold, 12, {x, y, w, 20}, "Theme", T.text);
                    y += 32;
                    const char* opts[] = {"light", "dark", "system"};
                    const char* names[] = {"Light", "Dark", "System"};
                    for (int i = 0; i < 3; i++) {
                        Rect card{x + float(i) * 150, y, 136, 96};
                        bool sel = settings.theme == opts[i];
                        auto pv = i == 0 ? ui::Theme::light() : i == 1 ? ui::Theme::dark_theme() : (win.is_dark_mode() ? ui::Theme::dark_theme() : ui::Theme::light());
                        r.rect(card, pv.bg, 8, sel ? 2 : 1, sel ? T.accent_emphasis : T.border);
                        r.rect({card.x + 10, card.y + 10, card.w - 20, 14}, pv.bg_toolbar, 3);
                        r.rect({card.x + 10, card.y + 30, 40, 40}, pv.bg_subtle, 3);
                        r.rect({card.x + 56, card.y + 30, card.w - 66, 8}, pv.diff_add_bg, 2);
                        r.rect({card.x + 56, card.y + 42, card.w - 66, 8}, pv.diff_del_bg, 2);
                        r.draw_text_in(FontId::Bold, 12, {card.x, card.bottom() - 24, card.w, 20}, names[i], pv.text, Align::Center);
                        if (ui.hovered(card)) ui.set_cursor(plat::Cursor::Hand);
                        if (ui.clicked(ui.id("theme", i), card)) { settings.theme = opts[i]; apply_theme(); save_state(); }
                    }
                    break;
                }
                case 4:
                    y = field_label(*this, x, y, w, "Default folder for cloned repositories");
                    path_picker(*this, x, y, w, settings.clone_dir, ui.id("set-clonedir"), "Choose a folder");
                    y += 40;
                    ui::checkbox_label(ui, ui.id("set-confirm-discard"), {x, y, w, 24}, settings.confirm_discard, "Confirm before discarding changes");
                    y += 30;
                    ui::checkbox_label(ui, ui.id("set-confirm-remove"), {x, y, w, 24}, settings.confirm_remove_repo, "Confirm before removing repositories");
                    y += 30;
                    ui::checkbox_label(ui, ui.id("set-confirm-force"), {x, y, w, 24}, settings.confirm_force_push, "Confirm before force pushing");
                    y += 40;
                    y = field_label(*this, x, y, w, "GitHub OAuth app client ID (enables browser sign-in)");
                    {
                        ui::TextFieldOpts o;
                        o.placeholder = "Optional — e.g. Ov23li…";
                        o.mono = true;
                        ui::text_field(ui, ui.id("set-clientid"), {x, y, w, 25}, settings.oauth_client_id, o);
                    }
                    y += 40;
                    ui::wrapped_text(ui, {x, y, w, 40}, "Register an OAuth app at github.com/settings/developers with Device Flow enabled, then paste its client ID.",
                                     FontId::Regular, 12, T.text_muted);
                    break;
            }
            bool ok = footer(*this, c, "Save", true, &cancel);
            ui::end_modal(ui);
            if (ok) {
                try {
                    auto cfg = git::Config::load(git::global_config_path());
                    bool changed = false;
                    if (!git::trim(settings_d.name).empty() && cfg.get("user.name") != git::trim(settings_d.name)) { cfg.set("user.name", git::trim(settings_d.name)); changed = true; }
                    if (!git::trim(settings_d.email).empty() && cfg.get("user.email") != git::trim(settings_d.email)) { cfg.set("user.email", git::trim(settings_d.email)); changed = true; }
                    if (!git::trim(settings_d.default_branch).empty()) {
                        settings.default_branch = git::trim(settings_d.default_branch);
                        if (cfg.get("init.defaultbranch") != settings.default_branch) { cfg.set("init.defaultBranch", settings.default_branch); changed = true; }
                    }
                    if (cfg.get_bool("commit.gpgsign", false) != settings_d.sign) { cfg.set("commit.gpgsign", settings_d.sign ? "true" : "false"); changed = true; }
                    if (settings_d.sign) {
                        const char* fm[] = {"openpgp", "ssh", "x509"};
                        if (cfg.get("gpg.format").value_or("openpgp") != fm[settings_d.sign_format]) {
                            if (settings_d.sign_format == 0) cfg.unset("gpg.format"); else cfg.set("gpg.format", fm[settings_d.sign_format]);
                            changed = true;
                        }
                        std::string k = git::trim(settings_d.signing_key);
                        if (cfg.get("user.signingkey").value_or("") != k) { if (k.empty()) cfg.unset("user.signingkey"); else cfg.set("user.signingkey", k); changed = true; }
                    }
                    if (changed) cfg.save();
                    load_git_identity();
                } catch (const std::exception& e) {
                    show_error("Couldn't save Git settings", e.what());
                    break;
                }
                save_state();
                dialog = Dialog::None;
            }
            if (closed || cancel) { dialog = Dialog::None; save_state(); }
            break;
        }

        // ------------------------------------------------------------ repo settings
        case Dialog::RepoSettings: {
            if (!repo_settings_d.loaded) {
                repo_settings_d.loaded = true;
                for (auto& rem : rs->remotes) if (rem.name == remote_name()) repo_settings_d.remote_url = rem.url;
                repo_settings_d.gitignore = git::try_read_file(git::git_to_path(rs->path) / ".gitignore").value_or("");
            }
            Rect c = ui::begin_modal(ui, ui.id("dlg-reposet"), "Repository settings", 640, 480, &closed);
            int nt = ui::tabs(ui, ui.id("rs-tabs"), {c.x, c.y, c.w, 40}, {"Remote", "Ignored files"}, repo_settings_d.tab);
            repo_settings_d.tab = nt;
            float x = c.x + 20, w = c.w - 40, y = c.y + 58;
            if (nt == 0) {
                if (rs->remotes.empty()) {
                    ui::wrapped_text(ui, {x, y, w, 40}, "This repository has no remote. Add one by entering a URL:", FontId::Regular, 12, T.text_muted);
                    y += 36;
                }
                y = field_label(*this, x, y, w, "Primary remote repository (" + remote_name() + ")");
                ui::TextFieldOpts o;
                o.mono = true;
                o.placeholder = "https://github.com/owner/repo.git";
                ui::text_field(ui, ui.id("rs-url"), {x, y, w, 25}, repo_settings_d.remote_url, o);
            } else {
                ui::wrapped_text(ui, {x, y, w, 40}, "Editing .gitignore. This file specifies intentionally untracked files that Git should ignore.",
                                 FontId::Regular, 12, T.text_muted);
                y += 40;
                ui::TextFieldOpts o;
                o.multiline = true;
                o.mono = true;
                ui::text_field(ui, ui.id("rs-gi"), {x, y, w, c.bottom() - 76 - y}, repo_settings_d.gitignore, o);
            }
            bool ok = footer(*this, c, "Save", true, &cancel);
            ui::end_modal(ui);
            if (ok) {
                auto d = repo_settings_d;
                auto rname = remote_name();
                bool had = !rs->remotes.empty();
                jobs.run("Save settings", [this, d, rname, had, path = rs->path] {
                    std::string err;
                    try {
                        auto repo = git::Repository::open(git::git_to_path(path));
                        if (!git::trim(d.remote_url).empty()) {
                            if (had) repo.set_remote_url(rname, git::trim(d.remote_url));
                            else repo.add_remote("origin", git::trim(d.remote_url));
                        }
                        auto gi = repo.worktree() / ".gitignore";
                        if (!d.gitignore.empty() || fs::exists(gi)) git::write_file(gi, d.gitignore);
                    } catch (const std::exception& e) { err = e.what(); }
                    jobs.post([this, err] { if (!err.empty()) show_error("Couldn't save settings", err); refresh(true); });
                });
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }

        // ------------------------------------------------------------ misc
        case Dialog::About: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-about"), "About Tributary", 480, 380, &closed);
            float x = c.x + 24, w = c.w - 48, y = c.y + 22;
            r.icon("git-branch", x + w / 2 - 16, y, 32, T.accent);
            y += 44;
            r.draw_text_in(FontId::Bold, 20, {x, y, w, 28}, "Tributary", T.text, Align::Center);
            y += 30;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, std::string("Version ") + kVersion + " (" + plat::os_name() + ")", T.text_muted, Align::Center);
            y += 34;
            y += ui::wrapped_text(ui, {x, y, w, 80}, "A from-scratch C++ reimplementation of GitHub Desktop: its own git engine, Vulkan renderer and UI toolkit. Released under the MIT License.",
                                  FontId::Regular, 12, T.text) + 10;
            ui::wrapped_text(ui, {x, y, w, 80}, "Uses Inter and JetBrains Mono (SIL Open Font License), GitHub Octicons (MIT), and GitHub Desktop's theme values and empty-state illustrations (MIT). Not affiliated with GitHub.",
                             FontId::Regular, 12, T.text_muted);
            bool ok = footer(*this, c, "Close", true, nullptr, ui::BtnStyle::Default);
            ui::end_modal(ui);
            if (ok || closed) dialog = Dialog::None;
            break;
        }
        case Dialog::Error: {
            float h = 150 + std::min(300.f, ui::wrapped_text(ui, {0, 0, 440, 0}, error_text, FontId::Regular, 12, T.text, 1.35f, true));
            Rect c = ui::begin_modal(ui, ui.id("dlg-error"), error_title, 520, h, &closed);
            Rect body{c.x + 20, c.y + 18, c.w - 40, c.h - 90};
            r.icon("alert", body.x, body.y + 2, 16, T.danger);
            r.push_clip(body);
            ui::wrapped_text(ui, {body.x + 26, body.y, body.w - 26, body.h}, error_text, FontId::Regular, 12, T.text);
            r.pop_clip();
            bool ok = footer(*this, c, "Close", true, nullptr, ui::BtnStyle::Default);
            ui::end_modal(ui);
            if (ok || closed) dialog = Dialog::None;
            break;
        }
        case Dialog::ConfirmDiscard: {
            size_t n = discard_d.paths.size();
            float h = 230 + std::min(float(n), 8.f) * 22;
            Rect c = ui::begin_modal(ui, ui.id("dlg-discard"), n == 1 ? "Confirm discard changes" : "Confirm discard all changes", 520, h, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 20},
                           n == 1 ? "Are you sure you want to discard all changes to:" : "Are you sure you want to discard all " + std::to_string(n) + " changed files?", T.text);
            y += 30;
            for (size_t i = 0; i < n && i < 8; i++) {
                r.draw_text_in(FontId::Mono, 12, {x + 12, y, w - 12, 20}, r.ellipsize_middle(FontId::Mono, 12, discard_d.paths[i], w - 12), T.text_muted);
                y += 22;
            }
            if (n > 8) { r.draw_text_in(FontId::Regular, 12, {x + 12, y, w, 20}, "…and " + std::to_string(n - 8) + " more", T.text_muted); y += 22; }
            y += 8;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, std::string("Changes can be restored by retrieving them from the ") +
                                                                    (plat::os_name() == "Windows" ? "Recycle Bin." : "Trash."), T.text_muted);
            bool dont = !settings.confirm_discard;
            if (ui::checkbox_label(ui, ui.id("discard-dont"), {x, c.bottom() - 96, w, 24}, dont, "Do not show this message again"))
                settings.confirm_discard = !dont;
            bool ok = footer(*this, c, n == 1 ? "Discard changes" : "Discard all changes", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { discard(discard_d.paths); dialog = Dialog::None; save_state(); }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::ConfirmRemoveRepo: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-remove"), "Remove repository", 480, 210, &closed);
            std::string name = remove_d.remove_index >= 0 && remove_d.remove_index < int(repos.size()) ? repos[size_t(remove_d.remove_index)].name : "";
            ui::wrapped_text(ui, {c.x + 20, c.y + 18, c.w - 40, 60},
                             "Remove \"" + name + "\" from Tributary? The repository will stay on your disk.", FontId::Regular, 12, T.text);
            bool ok = footer(*this, c, "Remove", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { remove_repo(remove_d.remove_index); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::CreateTag: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-tag"), "Create a tag", 480, 210, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            y = field_label(*this, x, y, w, "Name");
            ui::TextFieldOpts o;
            o.autofocus = true;
            o.placeholder = "v1.0.0";
            ui::text_field(ui, ui.id("tag-name"), {x, y, w, 25}, tag_d.name, o);
            bool ok = footer(*this, c, "Create tag", !git::trim(tag_d.name).empty(), &cancel);
            ui::end_modal(ui);
            if (ok) {
                auto name = git::trim(tag_d.name);
                auto target = tag_d.target;
                auto path = rs->path;
                jobs.run("Create tag", [this, name, target, path] {
                    std::string err;
                    try { git::Repository::open(git::git_to_path(path)).create_tag(name, target); } catch (const std::exception& e) { err = e.what(); }
                    jobs.post([this, err, name] {
                        if (!err.empty()) show_error("Couldn't create tag", err);
                        else toast("Created tag " + name + ". It will be pushed with your next push.");
                        refresh(true);
                    });
                });
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::ForcePush: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-force"), "Are you sure you want to force push?", 520, 230, &closed);
            ui::wrapped_text(ui, {c.x + 20, c.y + 18, c.w - 40, 80},
                             "A force push will rewrite history on " + remote_name() + "/" + rs->branch +
                                 ". Any collaborators working on this branch will need to reset their own local branch to match the history of the remote.",
                             FontId::Regular, 12, T.text);
            bool ok = footer(*this, c, "I'm sure", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { push(true); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::Conflicts: {
            if (!rs) { dialog = Dialog::None; break; }
            std::vector<const git::StatusEntry*> confl;
            for (auto& f : rs->status.files) if (f.conflicted) confl.push_back(&f);
            int unresolved = 0;
            for (auto* f : confl) if (f->has_markers) unresolved++;
            float h = 200 + std::min(float(confl.size()), 8.f) * 50;
            Rect c = ui::begin_modal(ui, ui.id("dlg-conflicts"), "Resolve conflicts before merging", 620, h, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 16;
            r.draw_text_in(FontId::Regular, 12, {x, y, w, 20},
                           unresolved ? std::to_string(unresolved) + " conflicted file" + (unresolved == 1 ? "" : "s") : "All conflicts resolved", T.text_muted);
            y += 30;
            int i = 0;
            for (auto* f : confl) {
                Rect row{x, y, w, 46};
                r.rect(row, T.bg_subtle, 6, 1, T.border);
                r.icon(f->has_markers ? "alert" : "check", row.x + 12, row.y + 15, 16, f->has_markers ? T.attention : T.success);
                r.draw_text_in(FontId::Bold, 12, {row.x + 38, row.y + 4, row.w - 300, 20}, f->path, T.text);
                r.draw_text_in(FontId::Regular, 12, {row.x + 38, row.y + 22, row.w - 300, 20},
                               f->has_markers ? "Has conflict markers" : "No conflicts remaining", T.text_muted);
                std::string p = f->path;
                if (ui::button(ui, ui.id("cf-edit", i), {row.right() - 256, row.y + 8, 120, 30}, "Open in editor"))
                    plat::open_in_editor(rs->path + "/" + p, settings.editor_cmd);
                Rect rb{row.right() - 128, row.y + 8, 116, 30};
                auto mid = ui.id("cf-menu", i);
                if (ui::button(ui, ui.id("cf-res", i), rb, "Resolve", ui::BtnStyle::Default, true, "chevron-down")) ui.open_popup(mid);
                Rect pm{rb.right() - 260, rb.bottom() + 4, 260, 72};
                if (ui::begin_popup(ui, mid, pm)) {
                    if (ui::menu_item(ui, ui.id("cf-ours", i), {pm.x, pm.y + 4, pm.w, 25}, "Use the modified file from " + (rs->branch.empty() ? "HEAD" : rs->branch)))
                        resolve_conflict(p, 2);
                    if (ui::menu_item(ui, ui.id("cf-theirs", i), {pm.x, pm.y + 36, pm.w, 25}, "Use the modified file from the other branch"))
                        resolve_conflict(p, 3);
                    ui::end_popup(ui);
                }
                y += 54;
                i++;
                if (y > c.bottom() - 110) break;
            }
            Rect f{c.x, c.bottom() - 65, c.w, 65};
            r.line_h(f.x, f.right(), f.y, T.border);
            bool rw = rs->status.rewrite.has_value();
            std::string what = rw ? rs->status.rewrite->label : rs->status.squashing ? "squash" : "merge";
            if (ui::button(ui, ui.id("cf-abort"), {f.x + 18, f.y + 20, 130, 25}, "Abort " + git::to_lower(what), ui::BtnStyle::Danger)) {
                if (rw) rewrite_abort(); else abort_merge();
                dialog = Dialog::None;
            }
            if (ui::button(ui, ui.id("cf-continue"), {f.right() - 170, f.y + 20, 152, 25}, "Continue " + git::to_lower(what), ui::BtnStyle::Primary, unresolved == 0)) {
                dialog = Dialog::None;
                if (rw) rewrite_continue();
                else {
                    for (auto& fs2 : rs->status.files) rs->sel[fs2.path].included = true;
                    if (rs->summary.empty()) rs->summary = git::trim(rs->status.merge_msg.substr(0, rs->status.merge_msg.find('\n')));
                    commit();
                }
            }
            ui::end_modal(ui);
            if (closed) dialog = Dialog::None;
            break;
        }
        case Dialog::SignIn: {
            bool ghe = ghe_d.signin_tab == 1;
            Rect c = ui::begin_modal(ui, ui.id("dlg-signin"), ghe ? "Sign in to GitHub Enterprise" : "Sign in to GitHub.com", 560,
                                     (settings.oauth_client_id.empty() || ghe ? 380 : 480) + 44 + (ghe ? 70 : 0), &closed);
            ghe_d.signin_tab = ui::tabs(ui, ui.id("signin-tabs"), {c.x, c.y, c.w, 40}, {"GitHub.com", "GitHub Enterprise"}, ghe_d.signin_tab);
            float x = c.x + 20, w = c.w - 40, y = c.y + 58;
            if (ghe) {
                y = field_label(*this, x, y, w, "Enterprise address");
                ui::TextFieldOpts eo;
                eo.placeholder = "https://github.example.com";
                ui::text_field(ui, ui.id("ghe-url"), {x, y, w, 25}, ghe_d.ghe_url, eo);
                y += 40;
            }
            if (!settings.oauth_client_id.empty() && !ghe) {
                if (signin_d.waiting && !signin_d.code.user_code.empty()) {
                    r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, signin_d.status, T.text_muted);
                    y += 30;
                    Rect code{x, y, w, 56};
                    r.rect(code, T.bg_subtle, 8, 1, T.border);
                    r.draw_text_in(FontId::Mono, 26, code, signin_d.code.user_code, T.text, Align::Center);
                    if (ui.clicked(ui.id("copy-code"), code)) { ui.set_clipboard(signin_d.code.user_code); toast("Code copied"); }
                    y += 66;
                    ui::spinner(ui, x + 10, y + 10, 6, T.text_muted);
                    r.draw_text_in(FontId::Regular, 12, {x + 26, y, w, 20}, "Waiting for you to authorize Tributary in your browser…", T.text_muted);
                    y += 34;
                } else {
                    if (!signin_d.status.empty()) { r.draw_text_in(FontId::Regular, 12, {x, y, w, 20}, signin_d.status, T.attention); y += 24; }
                    if (ui::button(ui, ui.id("signin-browser"), {x, y, w, 36}, "Continue with browser", ui::BtnStyle::Primary, !signin_d.waiting, "mark-github"))
                        start_device_flow();
                    y += 50;
                }
                r.line_h(x, x + w / 2 - 20, y + 8, T.border);
                r.line_h(x + w / 2 + 20, x + w, y + 8, T.border);
                r.draw_text_in(FontId::Regular, 12, {x + w / 2 - 20, y, 40, 16}, "or", T.text_muted, Align::Center);
                y += 26;
            }
            ui::wrapped_text(ui, {x, y, w, 60},
                             "Sign in with a personal access token. Create one with the repo, workflow, read:org and user scopes, then paste it below.",
                             FontId::Regular, 12, T.text_muted);
            y += 44;
            if (ui::button(ui, ui.id("signin-create"), {x, y, 220, 22}, "Create a token on GitHub", ui::BtnStyle::Link, true, "link-external")) {
                std::string base = "https://github.com";
                if (ghe && !git::trim(ghe_d.ghe_url).empty()) {
                    base = git::trim(ghe_d.ghe_url);
                    if (base.find("://") == std::string::npos) base = "https://" + base;
                    while (!base.empty() && base.back() == '/') base.pop_back();
                }
                plat::open_url(base + "/settings/tokens/new?scopes=repo,workflow,read:org,user&description=Tributary");
            }
            y += 32;
            y = field_label(*this, x, y, w, "Token");
            ui::TextFieldOpts o;
            o.password = true;
            o.mono = true;
            o.placeholder = "ghp_…";
            o.autofocus = settings.oauth_client_id.empty();
            bool enter = false;
            ui::text_field(ui, ui.id("signin-token"), {x, y, w, 25}, settings_d.token, o, &enter);
            bool tok_ok = !git::trim(settings_d.token).empty() && (!ghe || !git::trim(ghe_d.ghe_url).empty());
            bool ok = footer(*this, c, "Sign in", tok_ok && !jobs.busy(), &cancel) || (enter && tok_ok);
            ui::end_modal(ui);
            if (ok) { sign_in_with_token(settings_d.token); settings_d.token.clear(); }
            if (closed || cancel) { dialog = Dialog::None; signin_d.waiting = false; }
            break;
        }
        case Dialog::Credentials: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-cred"), "Authentication required", 520, 330, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::wrapped_text(ui, {x, y, w, 40}, "Enter your credentials for " + net::Url::parse(cred_d.url).host, FontId::Regular, 12, T.text_muted);
            y += 34;
            y = field_label(*this, x, y, w, "Username");
            ui::TextFieldOpts uo;
            uo.autofocus = true;
            ui::text_field(ui, ui.id("cred-user"), {x, y, w, 25}, cred_d.user, uo);
            y += 44;
            y = field_label(*this, x, y, w, "Password or token");
            ui::TextFieldOpts po;
            po.password = true;
            bool enter = false;
            ui::text_field(ui, ui.id("cred-pass"), {x, y, w, 25}, cred_d.pass, po, &enter);
            bool ok = footer(*this, c, "Continue", !cred_d.user.empty(), &cancel) || (enter && !cred_d.user.empty());
            ui::end_modal(ui);
            if (ok && cred_d.promise) {
                cred_d.promise->set_value(git::Credentials{cred_d.user, cred_d.pass});
                cred_d.promise = nullptr;
                dialog = Dialog::None;
            }
            if ((closed || cancel) && cred_d.promise) {
                cred_d.promise->set_value(std::nullopt);
                cred_d.promise = nullptr;
                dialog = Dialog::None;
            }
            break;
        }
        case Dialog::SwitchBranch: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-switch"), "Switch branch", 520, 300, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::wrapped_text(ui, {x, y, w, 40}, "You have changes on this branch. What would you like to do with them?", FontId::Regular, 12, T.text);
            y += 44;
            if (ui::radio(ui, ui.id("sw-leave"), {x, y, w, 26}, switch_d.choice == 0, "Leave my changes on " + (rs->branch.empty() ? "HEAD" : rs->branch))) switch_d.choice = 0;
            r.draw_text_in(FontId::Regular, 12, {x + 24, y + 24, w - 24, 18}, "Your in-progress work will be stashed on this branch for you to return to later.", T.text_muted);
            y += 56;
            if (ui::radio(ui, ui.id("sw-bring"), {x, y, w, 26}, switch_d.choice == 1, "Bring my changes to " + switch_d.target)) switch_d.choice = 1;
            r.draw_text_in(FontId::Regular, 12, {x + 24, y + 24, w - 24, 18}, "Your in-progress work will follow you to the new branch.", T.text_muted);
            bool ok = footer(*this, c, "Switch branch", true, &cancel);
            ui::end_modal(ui);
            if (ok) { dialog = Dialog::None; do_checkout(switch_d.target, switch_d.choice == 0); }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::Reword: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-reword"), "Reword commit", 560, 340, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::TextFieldOpts so;
            so.placeholder = "Summary (required)";
            so.autofocus = true;
            ui::text_field(ui, ui.id("rw-summary"), {x, y, w, 25}, reword_d.summary, so);
            y += 35;
            ui::TextFieldOpts dopt;
            dopt.multiline = true;
            dopt.placeholder = "Description";
            ui::text_field(ui, ui.id("rw-desc"), {x, y, w, c.bottom() - 85 - y}, reword_d.description, dopt);
            bool ok = footer(*this, c, "Reword commit", !git::trim(reword_d.summary).empty() && !jobs.busy(), &cancel);
            ui::end_modal(ui);
            if (ok) {
                std::string msg = git::trim(reword_d.summary) + "\n";
                if (!git::trim(reword_d.description).empty()) msg += "\n" + git::trim(reword_d.description) + "\n";
                reword_commit(reword_d.commit, msg);
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::Squash: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-squash"), "Squash " + std::to_string(squash_d.commits.size()) + " commits", 560, 380, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::TextFieldOpts so;
            so.placeholder = "Summary (required)";
            so.autofocus = true;
            ui::text_field(ui, ui.id("sq-summary"), {x, y, w, 25}, squash_d.summary, so);
            y += 35;
            ui::TextFieldOpts dopt;
            dopt.multiline = true;
            dopt.placeholder = "Description";
            ui::text_field(ui, ui.id("sq-desc"), {x, y, w, c.bottom() - 85 - y}, squash_d.description, dopt);
            bool ok = footer(*this, c, "Squash commits", !git::trim(squash_d.summary).empty() && !jobs.busy(), &cancel);
            ui::end_modal(ui);
            if (ok) {
                std::string msg = git::trim(squash_d.summary) + "\n";
                if (!git::trim(squash_d.description).empty()) msg += "\n" + git::trim(squash_d.description) + "\n";
                squash_commits(squash_d.commits, squash_d.into, msg);
                dialog = Dialog::None;
            }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::CherryPick: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-cherry"), "Cherry-pick " + std::to_string(cherry_d.commits.size()) + " commit" +
                                                                  (cherry_d.commits.size() == 1 ? "" : "s") + " to a branch", 520, 520, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 16;
            ui::TextFieldOpts fo;
            fo.placeholder = "Filter";
            fo.icon = "search";
            fo.autofocus = true;
            ui::text_field(ui, ui.id("cp-filter"), {x, y, w, 25}, cherry_d.filter, fo);
            y += 35;
            Rect list{x, y, w, c.bottom() - 75 - y};
            r.rect(list, T.bg, 6, 1, T.border);
            Rect inner = list.inset(1);
            float off = ui::begin_scroll(ui, ui.id("cp-list"), inner);
            float yy = inner.y - off;
            int i = 0;
            std::string f = git::to_lower(cherry_d.filter);
            for (auto& b : rs->branches) {
                if (b.current || b.remote) continue;
                if (!f.empty() && git::to_lower(b.name).find(f) == std::string::npos) continue;
                Rect row{inner.x, yy, inner.w, 30};
                if (ui::selectable(ui, ui.id("cpb", i++), row, cherry_d.selected == b.name)) cherry_d.selected = b.name;
                r.icon("git-branch", row.x + 12, row.y + 8, 16, T.text_muted);
                r.draw_text_in(FontId::Regular, 12, {row.x + 38, row.y, row.w - 50, row.h}, b.name, T.text);
                yy += 30;
            }
            ui::end_scroll(ui, ui.id("cp-list"), inner, yy + off - inner.y);
            bool ok = footer(*this, c, cherry_d.selected.empty() ? "Cherry-pick" : "Cherry-pick onto " + cherry_d.selected, !cherry_d.selected.empty(), &cancel);
            ui::end_modal(ui);
            if (ok) { cherry_pick(cherry_d.commits, cherry_d.selected); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::PrPreview: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-prprev"), "Open a pull request", std::min(1100.f, r.width() - 60), std::min(760.f, r.height() - 60), &closed);
            Rect head{c.x, c.y, c.w, 56};
            r.line_h(head.x, head.right(), head.bottom(), T.border);
            float x = head.x + 20;
            x += r.draw_text_in(FontId::Regular, 12, {x, head.y, 200, head.h}, "base:", T.text_muted) + 6;
            auto bpid = ui.id("pr-base");
            float bw = ui.text_w(FontId::Regular, 12, pr_d.base) + 40;
            Rect bb{x, head.y + 12, bw, 25};
            if (ui::button(ui, ui.id("pr-base-btn"), bb, pr_d.base, ui::BtnStyle::Default, true, "git-branch")) ui.open_popup(bpid);
            x = bb.right() + 12;
            r.icon("arrow-up", x, head.y + 20, 16, T.text_muted);
            r.draw_text_in(FontId::Regular, 12, {x + 22, head.y, 400, head.h}, "compare: " + rs->branch, T.text_muted);
            std::string summary = pr_d.loading ? "Loading…" : !pr_d.error.empty() ? pr_d.error :
                                  std::to_string(pr_d.commits) + " commit" + (pr_d.commits == 1 ? "" : "s") + ", " +
                                  std::to_string(pr_d.files.size()) + " changed file" + (pr_d.files.size() == 1 ? "" : "s");
            r.draw_text_in(FontId::Regular, 12, {head.right() - 320, head.y, 300, head.h}, summary, T.text_muted, Align::Right);
            Rect bp{bb.x, bb.bottom() + 4, 300, 320};
            if (ui::begin_popup(ui, bpid, bp)) {
                float yy = bp.y + 4;
                int i = 0;
                for (auto& b : rs->branches) {
                    if (b.current) continue;
                    if (ui::menu_item(ui, ui.id("prb", i++), {bp.x, yy, bp.w, 30}, b.name)) { pr_d.base = b.name; load_pr_preview(); }
                    yy += 30;
                    if (yy > bp.bottom() - 30) break;
                }
                ui::end_popup(ui);
            }
            Rect content{c.x, head.bottom() + 1, c.w, c.h - head.h - 62};
            float lw = std::min(300.f, content.w * 0.3f);
            Rect files{content.x, content.y, lw, content.h};
            r.line_v(files.right(), files.y, files.bottom(), T.border);
            float off = ui::begin_scroll(ui, ui.id("pr-files"), files);
            float yy = files.y - off;
            int i = 0;
            for (auto& fl : pr_d.files) {
                Rect row{files.x, yy, files.w, 30};
                if (ui::selectable(ui, ui.id("prf", i++), row, fl.path == pr_d.selected)) {
                    pr_d.selected = fl.path;
                    try {
                        auto repo = git::Repository::open(git::git_to_path(rs->path));
                        pr_d.diff = {};
                        pr_d.diff.path = fl.path;
                        pr_d.diff.diff = repo.commit_file_diff(fl);
                    } catch (...) {}
                }
                r.draw_text_in(FontId::Regular, 12, {row.x + 14, row.y, row.w - 50, row.h}, r.ellipsize_middle(FontId::Regular, 12, fl.path, row.w - 50), T.text);
                yy += 30;
            }
            ui::end_scroll(ui, ui.id("pr-files"), files, yy + off - files.y);
            if (!pr_d.selected.empty()) draw_diff({files.right() + 1, content.y, content.w - lw - 1, content.h}, pr_d.diff, false, pr_d.selected, "");
            else if (!pr_d.loading) r.draw_text_in(FontId::Regular, 14, {files.right(), content.y + content.h / 3, content.w - lw, 24},
                                                   "There are no changes.", T.text_muted, Align::Center);
            bool ok = footer(*this, c, "Create pull request", rs->github.has_value() && !rs->upstream.empty(), &cancel);
            if (!rs->github) r.draw_text_in(FontId::Regular, 12, {c.x + 20, c.bottom() - 46, 400, 25}, "This repository isn't on GitHub.", T.text_muted);
            else if (rs->upstream.empty()) r.draw_text_in(FontId::Regular, 12, {c.x + 20, c.bottom() - 46, 400, 25}, "Publish your branch first.", T.attention);
            ui::end_modal(ui);
            if (ok) { plat::open_url(github_url() + "/compare/" + pr_d.base + "..." + rs->branch + "?expand=1"); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::ResetToCommit: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-reset"), "Reset to commit", 520, 250, &closed);
            ui::wrapped_text(ui, {c.x + 20, c.y + 18, c.w - 40, 100},
                             "Reset " + rs->branch + " to " + reset_d.commit.short_hex() + "? The commits after it will be undone, and their changes "
                             "will appear in the Changes tab so you can commit them again.", FontId::Regular, 12, T.text);
            bool ok = footer(*this, c, "Reset", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { reset_to_commit(reset_d.commit); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::LargeFiles: {
            float h = 230 + std::min(float(large_d.files.size()), 6.f) * 22;
            Rect c = ui::begin_modal(ui, ui.id("dlg-large"), "Files too large", 560, h, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 18;
            ui::wrapped_text(ui, {x, y, w, 60}, "The following files are over 100 MB. If you commit them, you won't be able to push to GitHub. "
                                                "Consider Git LFS or adding them to .gitignore.", FontId::Regular, 12, T.text);
            y += 56;
            for (size_t i = 0; i < large_d.files.size() && i < 6; i++, y += 22)
                r.draw_text_in(FontId::Mono, 12, {x + 12, y, w - 12, 20}, large_d.files[i].first + "  (" + human_size(large_d.files[i].second) + ")", T.text_muted);
            bool ok = footer(*this, c, "Commit anyway", true, &cancel, ui::BtnStyle::Danger);
            bool use_lfs = ui::button(ui, ui.id("large-lfs"), {c.x + 20, c.bottom() - 45, 170, 25}, "Track with Git LFS", ui::BtnStyle::Primary);
            ui::end_modal(ui);
            if (use_lfs) {
                // track each big file's extension (or the file itself), then commit
                std::set<std::string> pats;
                for (auto& [p, sz] : large_d.files) {
                    auto dot = p.find_last_of('.'), sl = p.find_last_of('/');
                    pats.insert(dot != std::string::npos && (sl == std::string::npos || dot > sl) ? "*" + p.substr(dot) : p);
                }
                auto path = rs->path;
                dialog = Dialog::None;
                jobs.run("Track with Git LFS", [this, path, pats] {
                    std::string err;
                    try {
                        auto repo = git::Repository::open(git::git_to_path(path));
                        for (auto& pt : pats) git::lfs::track(repo, pt);
                    } catch (const std::exception& e) { err = e.what(); }
                    jobs.post([this, err] {
                        if (!err.empty()) { show_error("Couldn't set up Git LFS", err); return; }
                        if (rs) rs->sel[".gitattributes"].included = true;
                        refresh(true);
                        toast("These files will be stored with Git LFS. Review and commit.");
                    });
                });
            }
            if (ok) { commit(); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::ForkChoice: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-fork"), "How are you planning to use this fork?", 560, 300, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 20;
            if (ui::radio(ui, ui.id("fork-parent"), {x, y, w, 26}, fork_d.choice == 0, "To contribute to the parent project")) fork_d.choice = 0;
            r.draw_text_in(FontId::Regular, 12, {x + 24, y + 24, w - 24, 36}, "Pull requests and issues will target " + fork_d.parent + ".", T.text_muted);
            y += 60;
            if (ui::radio(ui, ui.id("fork-own"), {x, y, w, 26}, fork_d.choice == 1, "For my own purposes")) fork_d.choice = 1;
            r.draw_text_in(FontId::Regular, 12, {x + 24, y + 24, w - 24, 36}, "Everything stays in your fork.", T.text_muted);
            bool ok = footer(*this, c, "Continue", true, nullptr);
            ui::end_modal(ui);
            if (ok || closed) {
                auto d = fork_d;
                jobs.run("Fork setup", [this, d] {
                    try {
                        auto repo = git::Repository::open(git::git_to_path(d.path));
                        if (!repo.config().repo.get("remote.upstream.url")) repo.add_remote("upstream", d.parent_url);
                        repo.config().repo.set("tributary.forkContributionTarget", d.choice == 0 ? "parent" : "self");
                        repo.config().repo.save();
                        git::fetch(repo, "upstream", {}, {});
                    } catch (...) {}
                    jobs.post([this] { refresh(true); });
                });
                dialog = Dialog::None;
            }
            break;
        }
        case Dialog::ConfirmRewrite: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-rewrite"), "Rewrite pushed history?", 520, 240, &closed);
            ui::wrapped_text(ui, {c.x + 20, c.y + 18, c.w - 40, 100},
                             "Some of these commits have already been pushed. If you " + rewrite_d.what +
                                 " them, you'll need to force push, and anyone else working on this branch will have to reset to the new history.",
                             FontId::Regular, 12, T.text);
            bool ok = footer(*this, c, "Continue", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { dialog = Dialog::None; auto go = rewrite_d.go; if (go) go(); }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::DeleteTag: {
            Rect c = ui::begin_modal(ui, ui.id("dlg-deltag"), "Delete tag", 480, 220, &closed);
            ui::wrapped_text(ui, {c.x + 20, c.y + 18, c.w - 40, 40}, "Delete tag \"" + deltag_d.name + "\"?", FontId::Regular, 12, T.text);
            if (!rs->remotes.empty())
                ui::checkbox_label(ui, ui.id("deltag-remote"), {c.x + 20, c.y + 56, c.w - 40, 24}, deltag_d.remote, "Also delete it on " + remote_name());
            bool ok = footer(*this, c, "Delete", true, &cancel, ui::BtnStyle::Danger);
            ui::end_modal(ui);
            if (ok) { delete_tag(deltag_d.name, deltag_d.remote); dialog = Dialog::None; }
            if (closed || cancel) dialog = Dialog::None;
            break;
        }
        case Dialog::Checks: {
            auto& ch = rs->checks;
            float h = 160 + std::min(float(ch.runs.size()), 10.f) * 40;
            Rect c = ui::begin_modal(ui, ui.id("dlg-checks"), "Checks for " + (rs->head ? rs->head->short_hex() : std::string()), 600, h, &closed);
            float x = c.x + 20, w = c.w - 40, y = c.y + 14;
            if (ch.runs.empty()) r.draw_text_in(FontId::Regular, 12, {x, y, w, 24}, rs->checks_loading ? "Loading…" : "No checks for this commit.", T.text_muted);
            int i = 0;
            bool any_failed = false;
            for (auto& run : ch.runs) {
                if (y > c.bottom() - 100) break;
                Rect row{x, y, w, 36};
                bool hov = ui.hovered(row);
                if (hov) { r.rect(row, T.row_hover, 6); ui.set_cursor(plat::Cursor::Hand); }
                const char* ic = run.failed() ? "x" : run.pending() ? "dot-fill" : "check";
                Color col = run.failed() ? T.danger : run.pending() ? T.attention : T.success;
                any_failed |= run.failed();
                r.icon(ic, row.x + 8, row.y + 10, 16, col);
                r.draw_text_in(FontId::Bold, 12, {row.x + 34, row.y, row.w - 200, row.h}, run.name, T.text);
                std::string st = run.pending() ? (run.status == "queued" ? "Queued" : "In progress") : run.conclusion;
                if (!st.empty()) st[0] = char(std::toupper((unsigned char)st[0]));
                r.draw_text_in(FontId::Regular, 12, {row.right() - 160, row.y, 150, row.h}, st + (run.app.empty() ? "" : " · " + run.app), T.text_muted, Align::Right);
                if (ui.clicked(ui.id("chk", i++), row) && !run.details_url.empty()) plat::open_url(run.details_url);
                y += 40;
            }
            Rect f{c.x, c.bottom() - 65, c.w, 65};
            r.line_h(f.x, f.right(), f.y, T.border);
            if (ui::button(ui, ui.id("chk-rerun"), {f.x + 18, f.y + 20, 170, 25}, "Re-run failed checks", ui::BtnStyle::Default, any_failed, "sync")) rerun_checks();
            if (ui::button(ui, ui.id("chk-close"), {f.right() - 108, f.y + 20, 90, 25}, "Close")) dialog = Dialog::None;
            ui::end_modal(ui);
            if (closed) dialog = Dialog::None;
            break;
        }
        case Dialog::Welcome: dialog = Dialog::None; break;
        case Dialog::IdeOpen:
        case Dialog::IdeSubmit:
        case Dialog::IdeNewFile:
        case Dialog::IdeConfirmDiscard: draw_ide_dialogs(); break;
    }
}

}  // namespace app
