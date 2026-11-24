// SPDX-License-Identifier: MIT
// Main window views: toolbar, sidebar (changes/history), diff, blank slates.
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "../git/lfs.hpp"
#include "../gfx/embedded.hpp"
#include "../gfx/image.hpp"
#include "../net/http.hpp"
#include <thread>

namespace app {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using plat::Key;
using ui::Id;
namespace fs = std::filesystem;

static std::pair<std::string, std::string> split_path(const std::string& p) {
    std::string s = p;
    bool dir = !s.empty() && s.back() == '/';
    if (dir) s.pop_back();
    auto slash = s.rfind('/');
    if (slash == std::string::npos) return {"", s + (dir ? "/" : "")};
    return {s.substr(0, slash + 1), s.substr(slash + 1) + (dir ? "/" : "")};
}

static const char* change_icon(git::Change c) {
    switch (c) {
        case git::Change::Added: case git::Change::Untracked: return "diff-added";
        case git::Change::Deleted: return "diff-removed";
        case git::Change::Renamed: return "diff-renamed";
        case git::Change::Conflicted: return "alert";
        default: return "diff-modified";
    }
}

static Color change_color(const ui::Theme& T, git::Change c) {
    switch (c) {
        case git::Change::Added: case git::Change::Untracked: return T.success;
        case git::Change::Deleted: return T.danger;
        case git::Change::Renamed: return T.accent_emphasis;
        case git::Change::Conflicted: return T.conflicted;
        default: return T.modified;
    }
}

static std::string expand_tabs(const std::string& s) {
    if (s.find('\t') == std::string::npos) return s;
    std::string out;
    int col = 0;
    for (char ch : s) {
        if (ch == '\t') { int n = 4 - col % 4; out.append(size_t(n), ' '); col += n; }
        else { out += ch; if ((ch & 0xC0) != 0x80) col++; }
    }
    return out;
}

void App::open_context(std::vector<MenuEntry> items) {
    ctx_items = std::move(items);
    ctx_x = ui.mx;
    ctx_y = ui.my;
    ui.open_popup(ui.id("context-menu"));
}

void App::draw_context_menu() {
    auto pid = ui.id("context-menu");
    if (!ui.popup_open(pid)) return;
    float w = 260, h = 8;
    for (auto& it : ctx_items) h += it.separator ? 9 : 30;
    float x = std::min(ctx_x, r.width() - w - 4), y = std::min(ctx_y, r.height() - h - 4);
    if (ui::begin_popup(ui, pid, {x, y, w, h})) {
        float yy = y + 4;
        int i = 0;
        for (auto& it : ctx_items) {
            if (it.separator) { ui::separator(ui, {x + 8, yy, w - 16, 9}); yy += 9; continue; }
            if (ui::menu_item(ui, ui.id("ctx", i++), {x, yy, w, 30}, it.label, it.shortcut, it.enabled)) {
                auto fn = it.fn;
                if (fn) fn();
            }
            yy += 30;
        }
        ui::end_popup(ui);
    }
}

// ================================================================ toolbar
static void toolbar_section(App& a, const Rect& rc, const char* icon, const std::string& caption, const std::string& title,
                            bool dropdown, bool open, bool busy = false) {
    // ui/toolbar/_button.scss: padding 10, icon 16 + 10, title 12 semibold, description 11
    auto& T = a.ui.theme;
    Rect inner{rc.x, rc.y, rc.w - 1, rc.h};
    bool hov = a.ui.hovered(rc);
    if (open) a.r.rect(inner, T.bg);
    else if (hov) a.r.rect(inner, T.toolbar_hover);
    Color fg = open ? T.text : T.toolbar_text;
    Color dim = open ? T.text_muted : T.toolbar_text_dim;
    if (busy) ui::spinner(a.ui, rc.x + 18, rc.y + rc.h / 2, 6, fg);
    else a.r.icon(icon, rc.x + 10, rc.y + (rc.h - 16) / 2, 16, fg);
    float tx = rc.x + 36, tw = rc.w - 36 - (dropdown ? 36 : 10);
    a.r.draw_text_in(FontId::Regular, 11, {tx, rc.y + 8, tw, 16}, a.r.ellipsize(FontId::Regular, 11, caption, tw), dim);
    a.r.draw_text_in(FontId::Bold, 12, {tx, rc.y + 24, tw, 18}, a.r.ellipsize(FontId::Bold, 12, title, tw), fg);
    if (dropdown) a.r.icon("triangle-down", rc.right() - 27, rc.y + (rc.h - 16) / 2, 16, fg);
    a.r.line_v(rc.right() - 1, rc.y, rc.bottom(), T.toolbar_border);
    if (hov) a.ui.set_cursor(plat::Cursor::Hand);
}

void App::draw_toolbar(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg_toolbar);
    float W = rc.w;
    // the repository button is as wide as the sidebar; branch and push/pull are 230px
    float sec = std::clamp(settings.sidebar_width, 220.f, std::max(260.f, W * 0.5f));
    if (!rs) sec = 250;
    Rect repo_r{0, rc.y, sec + 1, rc.h - 1}, branch_r{sec + 1, rc.y, 230, rc.h - 1}, act_r{sec + 231, rc.y, 230, rc.h - 1};
    r.line_h(rc.x, rc.right(), rc.bottom() - 1, T.dark ? T.border : T.bg_toolbar);
    auto repo_pid = ui.id("repo-popup"), branch_pid = ui.id("branch-popup");

    toolbar_section(*this, repo_r, "repo", "Current repository", rs ? rs->name : "Select a repository", true,
                    ui.popup_open(repo_pid));
    if (ui.clicked(ui.id("tb-repo"), repo_r)) {
        if (ui.popup_open(repo_pid)) ui.close_popup();
        else { ui.open_popup(repo_pid); repo_filter.clear(); load_repo_indicators(); }
    }
    if (rs) {
        std::string bname = rs->branch.empty() ? (rs->head ? rs->head->short_hex() : "—") : rs->branch;
        std::string cap = rs->branch.empty() ? "Detached HEAD" : rs->status.merging ? "Merging into" : "Current branch";
        if (!rs->head && !rs->branch.empty()) cap = "Current branch (no commits)";
        if (hdrag.active) cap = "Drop to cherry-pick onto a branch";
        toolbar_section(*this, branch_r, "git-branch", cap, bname, true, ui.popup_open(branch_pid) || (hdrag.active && ui.hovered(branch_r)));
        // pull request badge + CI status for the current branch
        {
            float bx = branch_r.right() - 36;
            const gh::PullRequest* pr = nullptr;
            for (auto& p2 : rs->prs) if (p2.head_ref == rs->branch && !rs->branch.empty()) pr = &p2;
            if (rs->checks.state != gh::Checks::None) {
                Rect cr{bx - 18, branch_r.y + 17, 16, 16};
                const char* ic = rs->checks.state == gh::Checks::Success ? "check" : rs->checks.state == gh::Checks::Failure ? "x" : "dot-fill";
                Color cc = rs->checks.state == gh::Checks::Success ? T.success : rs->checks.state == gh::Checks::Failure ? T.danger : T.attention;
                r.icon(ic, cr.x, cr.y, 16, cc);
                if (ui.hovered(cr.inset(-4))) {
                    int ok = 0, bad = 0, pend = 0;
                    for (auto& c : rs->checks.runs) { if (c.failed()) bad++; else if (c.pending()) pend++; else ok++; }
                    ui.tooltip(ui.id("checks-tip"), cr, std::to_string(ok) + " passed, " + std::to_string(bad) + " failed, " + std::to_string(pend) + " pending — click for details");
                }
                if (ui.clicked(ui.id("checks-btn"), cr.inset(-4))) { dialog = Dialog::Checks; load_checks(true); }
                bx -= 24;
            }
            if (pr) {
                // ui/_pull-request-badge.scss
                std::string t = "#" + std::to_string(pr->number);
                float w = ui.text_w(FontId::Bold, 9, t) + 26;
                Rect pb{bx - w, branch_r.y + 18, w, 14};
                r.rect(pb, T.toolbar_active, 8);
                r.icon("git-pull-request", pb.x + 5, pb.y + 2, 10, T.toolbar_text);
                r.draw_text_in(FontId::Bold, 9, {pb.x + 17, pb.y, w - 18, 14}, t, T.toolbar_text);
                if (ui.hovered(pb)) ui.tooltip(ui.id("pr-tip"), pb, pr->title);
                if (ui.clicked(ui.id("pr-badge"), pb)) plat::open_url(pr->html_url);
            }
        }
        if (hdrag.active && ui.hovered(branch_r) && !ui.popup_open(branch_pid)) { ui.open_popup(branch_pid); branch_filter.clear(); branch_tab = 0; }
        if (!hdrag.active && ui.clicked(ui.id("tb-branch"), branch_r)) {
            if (ui.popup_open(branch_pid)) ui.close_popup();
            else { ui.open_popup(branch_pid); branch_filter.clear(); load_prs(); refresh(true); }
        }
        // action button
        std::string label = jobs.current_label();
        bool busy = jobs.busy() && (label == "Fetch" || label == "Pull" || label == "Push" || label == "Publish branch" ||
                                    label == "Publish repository");
        std::string remote = remote_name();
        std::string fetched = rs->last_fetched ? "Last fetched " + git::format_relative_time(rs->last_fetched, git::unix_now())
                                               : "Never fetched";
        const char* icon = "sync";
        std::string title;
        cap.clear();
        std::function<void()> action;
        if (rs->remotes.empty()) {
            icon = "upload";
            title = "Publish repository";
            cap = "Publish this repository to GitHub";
            action = [this] {
                if (!account.valid()) { dialog = Dialog::SignIn; return; }
                publish_d = {};
                publish_d.name = rs->name;
                dialog = Dialog::Publish;
                auto acc = account;
                std::thread([this, acc] {
                    std::vector<std::string> orgs;
                    try { orgs = gh::list_orgs(acc); } catch (...) {}
                    jobs.post([this, orgs] { publish_d.orgs = orgs; });
                }).detach();
            };
        } else if (!rs->branch.empty() && rs->upstream.empty() && rs->head) {
            icon = "upload";
            title = "Publish branch";
            cap = "Publish this branch to " + remote;
            action = [this] { publish_branch(); };
        } else if (rs->behind > 0) {
            icon = "arrow-down";
            title = "Pull " + remote;
            cap = fetched;
            action = [this] { pull(); };
        } else if (rs->ahead > 0) {
            icon = "arrow-up";
            title = "Push " + remote;
            cap = fetched;
            action = [this] { push(); };
        } else {
            title = "Fetch " + remote;
            cap = fetched;
            action = [this] { fetch(); };
        }
        if (busy) {
            auto [ptext, frac] = jobs.progress();
            cap = ptext.empty() ? label + "…" : ptext;
            title = label == "Fetch" ? "Fetching " + remote : label == "Pull" ? "Pulling " + remote
                    : label == "Push" ? "Pushing to " + remote : label + "…";
        }
        toolbar_section(*this, act_r, icon, cap, title, false, false, busy);
        if (!busy) {
            // .ahead-behind: one pill, "1↑ 2↓", 9px semibold, 6px padding, radius 8
            bool a2 = rs->ahead > 0 && !rs->upstream.empty(), b2 = rs->behind > 0 && !rs->upstream.empty();
            if (a2 || b2) {
                std::string sa = std::to_string(rs->ahead), sb = std::to_string(rs->behind);
                float w = 12;
                if (a2) w += ui.text_w(FontId::Bold, 9, sa) + 13;
                if (b2) w += ui.text_w(FontId::Bold, 9, sb) + 13 + (a2 ? 5 : 0);
                Rect pill{act_r.right() - 10 - w, act_r.y + 18, w, 14};
                r.rect(pill, T.toolbar_active, 8);
                float px = pill.x + 6;
                if (a2) {
                    px += r.draw_text_in(FontId::Bold, 9, {px, pill.y, 40, 14}, sa, T.toolbar_text) + 3;
                    r.icon("arrow-up", px, pill.y + 2, 10, T.toolbar_text);
                    px += 15;
                }
                if (b2) {
                    px += r.draw_text_in(FontId::Bold, 9, {px, pill.y, 40, 14}, sb, T.toolbar_text) + 3;
                    r.icon("arrow-down", px, pill.y + 2, 10, T.toolbar_text);
                }
            }
        }
        if (!busy && ui.clicked(ui.id("tb-action"), act_r) && action) action();
        if (!busy && ui.hovered(act_r) && ui.mouse_pressed[1] && !rs->remotes.empty()) {
            open_context({
                {"Fetch " + remote, [this] { fetch(); }},
                {"Pull " + remote, [this] { pull(); }, !rs->upstream.empty()},
                {"Push " + remote, [this] { push(); }, rs->ahead > 0 || rs->upstream.empty()},
                {"", {}, true, true},
                {"Force push " + remote + "…", [this] { dialog = Dialog::ForcePush; }, !rs->upstream.empty()},
            });
        }
    }
    draw_repo_popup({0, rc.bottom(), sec, 0});
    if (rs) draw_branch_popup({branch_r.x, rc.bottom(), sec, 0});
    draw_context_menu();
}

// ================================================================ repository list
void App::draw_repo_popup(const Rect& anchor) {
    auto pid = ui.id("repo-popup");
    if (!ui.popup_open(pid)) return;
    auto& T = ui.theme;
    // ui/_foldout.scss: square panel hanging from the toolbar to the window bottom
    Rect pr{anchor.x, anchor.y, std::max(anchor.w, 365.f), r.height() - anchor.y};
    if (!ui::begin_popup(ui, pid, pr, T.bg, 0.f)) return;
    Rect top{pr.x + 10, pr.y + 10, pr.w - 20, 25};
    Rect addb = top.cut_right(70);
    ui::TextFieldOpts fo;
    fo.placeholder = "Filter";
    fo.icon = "search";
    fo.autofocus = true;
    ui::text_field(ui, ui.id("repo-filter"), {top.x, top.y, top.w - 5, top.h}, repo_filter, fo);
    auto add_pid = ui.id("repo-add-menu");
    if (ui::button(ui, ui.id("repo-add"), addb, "Add", ui::BtnStyle::Default, true)) ui.open_popup(add_pid);
    r.icon("triangle-down", addb.right() - 22, addb.y + 4, 16, T.text);
    Rect list{pr.x, top.bottom() + 10, pr.w, pr.bottom() - top.bottom() - 10};
    // pinned repositories first, then grouped by owner
    std::vector<int> pinned;
    std::map<std::string, std::vector<int>> by_owner;
    std::string f = git::to_lower(repo_filter);
    for (int i = 0; i < int(repos.size()); i++) {
        auto& e = repos[size_t(i)];
        if (!f.empty() && git::to_lower(e.name).find(f) == std::string::npos && git::to_lower(e.owner).find(f) == std::string::npos) continue;
        if (e.pinned) pinned.push_back(i);
        else by_owner[e.owner.empty() ? "\xff" "Other" : e.owner].push_back(i);
    }
    std::vector<std::pair<std::string, std::vector<int>>> groups;
    if (!pinned.empty()) groups.push_back({"Pinned", pinned});
    for (auto& [owner, idxs] : by_owner) groups.push_back({owner == "\xff" "Other" ? "Other" : owner, idxs});
    float off = ui::begin_scroll(ui, ui.id("repo-list"), list);
    float y = list.y - off;
    int nav = 0;
    for (auto& [title, idxs] : groups) {
        bool pin_group = title == "Pinned" && !pinned.empty() && idxs == pinned;
        // .filter-list-group-header: 29px, semibold, 10px padding
        if (pin_group) r.icon("pin", list.x + 10, y + 7, 14, T.text);
        r.draw_text_in(FontId::Bold, 12, {list.x + (pin_group ? 30 : 10), y, list.w, 29}, title, T.text);
        y += 29;
        for (int i : idxs) {
            auto& e = repos[size_t(i)];
            Rect row{list.x, y, list.w, 29};
            Rect pin_r{row.right() - 32, row.y + 3, 24, 23};
            bool sel = i == current;
            bool rc_click = false;
            bool row_hov = ui.hovered(row);
            bool pin_hov = ui.hovered(pin_r);
            bool pin_click = ui.clicked(ui.id("repo-pin", i), pin_r);
            if (ui::selectable(ui, ui.id("repo-row", i), {row.x, row.y, row.w - (pin_hov ? 34 : 0), row.h}, sel, nullptr, &rc_click) && !pin_hov) {
                open_repo(i);
                ui.close_popup();
            }
            if (sel || row_hov) {}  // background already drawn by selectable
            r.icon(e.missing ? "alert" : "repo", row.x + 10, row.y + 6, 16, e.missing ? T.attention : T.text);
            float nw = r.draw_text_in(FontId::Regular, 12, {row.x + 36, row.y, row.w - 120, row.h}, e.name, T.text);
            if (e.indicators) {
                float ix = row.x + 34 + nw + 8;
                // .change-indicator (dot-fill) and the ahead/behind pill, as in the repository list
                if (e.changes) { r.icon("dot-fill", ix, row.y + 6, 16, T.accent_emphasis); ix += 20; }
                if (e.ahead || e.behind) {
                    std::string t = (e.ahead ? std::to_string(e.ahead) + "\u2191" : "") + (e.ahead && e.behind ? " " : "") +
                                    (e.behind ? std::to_string(e.behind) + "\u2193" : "");
                    float tw2 = ui.text_w(FontId::Bold, 9, t) + 12;
                    r.rect({ix, row.y + 7, tw2, 15}, T.badge_bg, 8);
                    r.draw_text_in(FontId::Bold, 9, {ix, row.y + 7, tw2, 15}, t, T.text, Align::Center);
                }
            }
            // pin toggle: always visible when pinned, on hover otherwise
            if (e.pinned || row_hov) {
                if (pin_hov) {
                    r.rect(pin_r, T.row_selected, 5);
                    ui.set_cursor(plat::Cursor::Hand);
                    ui.tooltip(ui.id("repo-pin-tip", i), pin_r, e.pinned ? "Unpin" : "Pin to top");
                }
                const char* ic = e.pinned && pin_hov ? "pin-slash" : "pin";
                r.icon(ic, pin_r.x + 4, pin_r.y + 4, 16, e.pinned ? T.accent : T.text_muted);
            }
            if (pin_click) { toggle_pin(i); break; }
            if (rc_click) {
                int idx = i;
                std::string path = e.path;
                open_context({
                    {"Open in terminal", [this, path] { plat::open_in_terminal(path, settings.shell_cmd); }},
                    {"Show in file manager", [path] { plat::reveal_in_file_manager(path); }},
                    {"Open in editor", [this, path] { plat::open_in_editor(path, settings.editor_cmd); }},
                    {"Copy repository path", [this, path] { ui.set_clipboard(path); }},
                    {"", {}, true, true},
                    {e.pinned ? "Unpin" : "Pin to top", [this, idx] { toggle_pin(idx); }},
                    {"Remove…", [this, idx] {
                        if (settings.confirm_remove_repo) { remove_d.remove_index = idx; dialog = Dialog::ConfirmRemoveRepo; }
                        else remove_repo(idx);
                    }},
                });
            }
            y += 29;
            nav++;
        }
    }
    if (repos.empty()) {
        r.draw_text_in(FontId::Regular, 12, {list.x, y + 20, list.w, 24}, "No repositories yet.", T.text_muted, Align::Center);
        y += 60;
    }
    ui::end_scroll(ui, ui.id("repo-list"), list, y + off - list.y + 8);
    ui::end_popup(ui);
    // add menu
    Rect am{addb.x, addb.bottom() + 2, 230, 96};
    if (ui::begin_popup(ui, add_pid, am)) {
        if (ui::menu_item(ui, ui.id("am-clone"), {am.x, am.y + 3, am.w, 30}, "Clone repository…", "")) { ui.close_popup(); clone_d = {}; clone_d.tab = account.valid() ? 0 : 1; dialog = Dialog::Clone; load_gh_repos(); }
        if (ui::menu_item(ui, ui.id("am-create"), {am.x, am.y + 33, am.w, 30}, "Create new repository…", "")) { ui.close_popup(); create_d = {}; create_d.path = settings.clone_dir; dialog = Dialog::Create; }
        if (ui::menu_item(ui, ui.id("am-add"), {am.x, am.y + 63, am.w, 30}, "Add existing repository…", "")) { ui.close_popup(); add_d = {}; dialog = Dialog::AddExisting; }
        ui::end_popup(ui);
    }
}

// ================================================================ branches
void App::draw_branch_popup(const Rect& anchor) {
    auto pid = ui.id("branch-popup");
    if (!ui.popup_open(pid)) return;
    auto& T = ui.theme;
    Rect pr{anchor.x, anchor.y, 365, r.height() - anchor.y};  // .branches-container { width: 365px }
    if (hdrag.active) hdrag.branch_target.clear();  // re-set below while hovering a branch row
    if (!ui::begin_popup(ui, pid, pr, T.bg, 0.f)) return;
    std::vector<std::string> tab_labels = {"Branches", "Pull requests"};
    std::vector<std::string> badges = {"", rs->prs.empty() ? "" : std::to_string(rs->prs.size())};
    branch_tab = ui::tabs(ui, ui.id("branch-tabs"), {pr.x, pr.y, pr.w, 29}, tab_labels, branch_tab, badges);
    Rect top{pr.x + 10, pr.y + 39, pr.w - 20, 25};
    if (branch_tab == 0) {
        Rect nb = top.cut_right(100);
        ui::TextFieldOpts fo;
        fo.placeholder = "Filter";
        fo.icon = "search";
        fo.autofocus = true;
        bool enter = false;
        ui::text_field(ui, ui.id("branch-filter"), {top.x, top.y, top.w - 5, top.h}, branch_filter, fo, &enter);
        if (ui::button(ui, ui.id("branch-new"), nb, "New branch")) {
            ui.close_popup();
            branch_d = {};
            branch_d.name = git::sanitize_branch_name(branch_filter);
            dialog = Dialog::NewBranch;
        }
        Rect list{pr.x, top.bottom() + 10, pr.w, pr.bottom() - top.bottom() - 10 - 46};
        std::string f = git::to_lower(branch_filter);
        std::vector<const git::BranchInfo*> def, local, remote_only;
        std::set<std::string> local_names;
        for (auto& b : rs->branches) if (!b.remote) local_names.insert(b.name);
        std::string defb = rs->default_branch.empty() ? "main" : rs->default_branch;
        for (auto& b : rs->branches) {
            if (!f.empty() && git::to_lower(b.name).find(f) == std::string::npos) continue;
            if (!b.remote) (b.name == defb ? def : local).push_back(&b);
            else {
                auto slash = b.name.find('/');
                std::string bare = b.name.substr(slash + 1);
                if (!local_names.count(bare)) remote_only.push_back(&b);
            }
        }
        std::sort(local.begin(), local.end(), [](auto a, auto b) { return a->tip_time > b->tip_time; });
        std::sort(remote_only.begin(), remote_only.end(), [](auto a, auto b) { return a->tip_time > b->tip_time; });
        float off = ui::begin_scroll(ui, ui.id("branch-list"), list);
        float y = list.y - off;
        int n = 0;
        auto section = [&](const std::string& title, const std::vector<const git::BranchInfo*>& v) {
            if (v.empty()) return;
            r.draw_text_in(FontId::Bold, 12, {list.x + 10, y, list.w, 29}, title, T.text);
            y += 29;
            for (auto* b : v) {
                Rect row{list.x, y, list.w, 30};
                if (row.bottom() >= list.y && row.y <= list.bottom()) {
                    bool rc_click = false;
                    bool cur = b->current;
                    if (hdrag.active) {
                        bool over = ui.hovered(row) && !b->remote && !cur;
                        if (over) { r.rect(row, T.accent_emphasis.with_alpha(0.25f), 6, 1, T.accent_emphasis); hdrag.branch_target = b->name; }
                    } else if (ui::selectable(ui, ui.id("br", n), row, cur, nullptr, &rc_click) && !cur) {
                        ui.close_popup();
                        checkout(b->name);
                    }
                    // .branches-list-item: icon (check for current), name, relative date in secondary text
                    r.icon(cur ? "check" : "git-branch", row.x + 10, row.y + 7, 16, T.text);
                    float rt = b->tip_time ? ui.text_w(FontId::Regular, 12, git::format_relative_time(b->tip_time, git::unix_now())) : 0;
                    r.draw_text_in(FontId::Regular, 12, {row.x + 36, row.y, row.w - 56 - rt, row.h},
                                   r.ellipsize(FontId::Regular, 12, b->name, row.w - 56 - rt), T.text);
                    if (b->tip_time)
                        r.draw_text_in(FontId::Regular, 12, {row.right() - rt - 10, row.y, rt, row.h},
                                       git::format_relative_time(b->tip_time, git::unix_now()), T.text_muted, Align::Right);
                    if (rc_click) {
                        std::string name = b->name;
                        bool remote = b->remote;
                        std::vector<MenuEntry> items;
                        if (!remote) {
                            items.push_back({"Rename…", [this, name] { rename_d = {name, name}; dialog = Dialog::RenameBranch; }});
                            items.push_back({"Merge into current branch", [this, name] { merge_into_current("refs/heads/" + name); }, !cur});
                            items.push_back({"Rebase current branch onto " + name, [this, name] { rebase_onto("refs/heads/" + name); }, !cur});
                            items.push_back({"View on GitHub", [this, name] { plat::open_url(github_url() + "/tree/" + name); }, rs->github.has_value()});
                            items.push_back({"Copy branch name", [this, name] { ui.set_clipboard(name); }});
                            items.push_back({"", {}, true, true});
                            items.push_back({"Delete…", [this, name] { delete_d = {name, false}; dialog = Dialog::DeleteBranch; }, !cur});
                        } else {
                            items.push_back({"Check out", [this, name] { checkout(name); }});
                            items.push_back({"Merge into current branch", [this, name] { merge_into_current("refs/remotes/" + name); }});
                            items.push_back({"Copy branch name", [this, name] { ui.set_clipboard(name); }});
                        }
                        ui.close_popup();
                        open_context(items);
                    }
                }
                y += 30;
                n++;
            }
        };
        section("Default branch", def);
        section("Recent branches", local);
        section("Remote branches", remote_only);
        if (def.empty() && local.empty() && remote_only.empty()) {
            r.draw_text_in(FontId::Regular, 12, {list.x, y + 20, list.w, 24},
                           f.empty() ? "No branches yet." : "No branches match \"" + branch_filter + "\".", T.text_muted, Align::Center);
            y += 64;
        }
        ui::end_scroll(ui, ui.id("branch-list"), list, y + off - list.y + 6);
        if (enter && !branch_filter.empty()) {
            ui.close_popup();
            branch_d = {};
            branch_d.name = git::sanitize_branch_name(branch_filter);
            dialog = Dialog::NewBranch;
        }
        // .merge-button-row
        Rect foot{pr.x, pr.bottom() - 46, pr.w, 46};
        r.line_h(foot.x, foot.right(), foot.y, T.border);
        Rect mb = {foot.x + 10, foot.y + 10, foot.w - 20, 25};
        bool mclick = ui::button(ui, ui.id("branch-merge"), mb, "", ui::BtnStyle::Default, !rs->branch.empty());
        {
            std::string pre = "Choose a branch to merge into ", bn = rs->branch.empty() ? "HEAD" : rs->branch;
            float w1 = ui.text_w(FontId::Regular, 12, pre), w2 = ui.text_w(FontId::Bold, 12, bn);
            float ix = std::round(mb.x + (mb.w - w1 - w2 - 22) / 2);
            r.icon("git-merge", ix, mb.y + 4, 16, T.text);
            r.draw_text_in(FontId::Regular, 12, {ix + 22, mb.y, w1 + 1, mb.h}, pre, T.text);
            r.draw_text_in(FontId::Bold, 12, {ix + 22 + w1, mb.y, w2 + 1, mb.h}, bn, T.text);
        }
        if (mclick) {
            ui.close_popup();
            merge_d = {};
            dialog = Dialog::Merge;
            refresh(true);
        }
    } else {
        Rect list{pr.x, pr.y + 29, pr.w, pr.h - 29};
        if (!rs->github) {
            ui::wrapped_text(ui, list.inset(24, 30), "Pull requests are available for repositories hosted on GitHub.", FontId::Regular, 12, T.text_muted);
        } else if (!account.valid()) {
            ui::wrapped_text(ui, list.inset(24, 30), "Sign in to GitHub to see this repository's pull requests.", FontId::Regular, 12, T.text_muted);
            if (ui::button(ui, ui.id("pr-signin"), {list.x + 24, list.y + 80, 120, 30}, "Sign in", ui::BtnStyle::Primary)) {
                ui.close_popup();
                dialog = Dialog::SignIn;
            }
        } else if (rs->prs_loading && rs->prs.empty()) {
            ui::spinner(ui, list.x + list.w / 2, list.y + 60, 8, T.text_muted);
        } else if (rs->prs.empty()) {
            r.draw_text_in(FontId::Bold, 14, {list.x, list.y + 30, list.w, 24}, "You're all set!", T.text, Align::Center);
            r.draw_text_in(FontId::Regular, 12, {list.x, list.y + 56, list.w, 24},
                           "No open pull requests in " + rs->github->first + "/" + rs->github->second, T.text_muted, Align::Center);
        } else {
            float off = ui::begin_scroll(ui, ui.id("pr-list"), list);
            float y = list.y - off;
            int i = 0;
            for (auto& pr2 : rs->prs) {
                Rect row{list.x + 4, y, list.w - 8, 52};
                if (ui::selectable(ui, ui.id("pr", i++), row, rs->branch == "pr/" + std::to_string(pr2.number))) {
                    ui.close_popup();
                    checkout_pr(pr2);
                }
                r.icon("git-pull-request", row.x + 10, row.y + 10, 16, pr2.draft ? T.text_muted : T.success);
                r.draw_text_in(FontId::Bold, 12, {row.x + 34, row.y + 6, row.w - 44, 20}, pr2.title, T.text);
                r.draw_text_in(FontId::Regular, 12, {row.x + 34, row.y + 26, row.w - 44, 18},
                               "#" + std::to_string(pr2.number) + " opened " + git::format_relative_time(pr2.created_at, git::unix_now()) +
                                   " by " + pr2.author + (pr2.draft ? " • Draft" : ""), T.text_muted);
                y += 52;
            }
            ui::end_scroll(ui, ui.id("pr-list"), list, y + off - list.y);
        }
    }
    ui::end_popup(ui);
}

// ================================================================ app menu
// GitHub Desktop's menus (app/src/main-process/menu/build-default-menu.ts), with
// the commands Tributary has.
std::vector<AppMenu> App::build_menus() {
    using K = Key;
    const uint8_t C = plat::ModCmd, S = plat::ModShift, A = plat::ModAlt;
    bool mac = plat::os_name() == "macOS", win = plat::os_name() == "Windows";
    bool has = rs != nullptr;
    bool br = has && !rs->branch.empty();
    std::string def = has ? (rs->default_branch.empty() ? "main" : rs->default_branch) : "main";
    auto sep = [] { AppMenuItem i; i.sep = true; return i; };
    auto it = [](std::string l, K k, uint8_t m, std::function<void()> f, bool en = true) {
        AppMenuItem i;
        i.label = std::move(l); i.key = k; i.mods = m; i.fn = std::move(f); i.enabled = en;
        return i;
    };
    auto key = [this](K k, uint8_t m) { return [this, k, m] { injected_keys.push_back({k, m}); }; };
    auto native = [](std::string l, int n, K k = K::Unknown, uint8_t m = 0) {
        AppMenuItem i;
        i.label = std::move(l); i.native = n; i.key = k; i.mods = m;
        return i;
    };
    auto open_settings = [this] { settings_d = {}; dialog = Dialog::Settings; };
    auto about = [this] { dialog = Dialog::About; };
    std::vector<AppMenu> m;
    if (mac) m.push_back({"Tributary", {it("About Tributary", K::Unknown, 0, about), sep(), it("Settings…", K::Comma, C, open_settings), sep(),
                                        native("Hide Tributary", -1, K::H, C), native("Hide Others", -2, K::H, uint8_t(C | A)),
                                        native("Show All", -3), sep(), native("Quit Tributary", -4, K::Q, C)}});
    AppMenu file{"File", {
        it("New repository…", K::N, C, [this] { create_d = {}; create_d.path = settings.clone_dir; dialog = Dialog::Create; }),
        sep(),
        it("Add local repository…", K::O, C, [this] { add_d = {}; dialog = Dialog::AddExisting; }),
        it("Clone repository…", K::O, uint8_t(C | S), [this] { clone_d = {}; clone_d.tab = account.valid() ? 0 : 1; dialog = Dialog::Clone; load_gh_repos(); }),
        sep(),
        it("Edit a repository on GitHub…", K::E, uint8_t(C | A), [this] {
            if (!account.valid()) { ghe_d.signin_tab = 0; dialog = Dialog::SignIn; return; }
            ide_open_d = {};
            dialog = Dialog::IdeOpen;
            load_gh_repos();
        }),
    }};
    if (!mac) {
        file.items.push_back(sep());
        file.items.push_back(it(win ? "Options…" : "Settings…", K::Comma, C, open_settings));
        file.items.push_back(sep());
        file.items.push_back(it("Exit", K::Unknown, 0, [this] { save_state(); should_quit = true; }));
    }
    m.push_back(file);
    if (mac) m.push_back({"Edit", {native("Undo", -5, K::Z, C), native("Redo", -6, K::Z, uint8_t(C | S)), sep(), native("Cut", -7, K::X, C),
                                   native("Copy", -8, K::C, C), native("Paste", -9, K::V, C), native("Select all", -10, K::A, C), sep(),
                                   it("Find", K::F, C, [this] { find_open = true; ui.focus = 0; }, has)}});
    else m.push_back({"Edit", {it("Undo", K::Z, C, key(K::Z, C)), it("Redo", K::Y, C, key(K::Y, C)), sep(), it("Cut", K::X, C, key(K::X, C)),
                              it("Copy", K::C, C, key(K::C, C)), it("Paste", K::V, C, key(K::V, C)), it("Select all", K::A, C, key(K::A, C)), sep(),
                              it("Find", K::F, C, [this] { find_open = true; ui.focus = 0; }, has)}});
    AppMenu view{"View", {
        it("Changes", K::Num1, C, [this] { tab = 0; }, has),
        it("History", K::Num2, C, [this] { tab = 1; if (!rs->selected_commit && !rs->history.empty()) select_commit(rs->history[0].id); }, has),
        it("Repository list", K::T, C, [this] { ui.open_popup(ui.id("repo-popup")); repo_filter.clear(); load_repo_indicators(); }),
        it("Branches list", K::B, C, [this] { ui.open_popup(ui.id("branch-popup")); branch_filter.clear(); load_prs(); }, has),
        sep(),
        it("Go to summary", K::G, C, [this] { tab = 0; ui.set_focus(summary_id); }, has),
        it("Show stashed changes", K::H, C, [this] { tab = 0; load_stash_view(); }, has && std::any_of(rs->stashes.begin(), rs->stashes.end(), [this](auto& s2) { return s2.branch == rs->branch; })),
    }};
    if (mac) view.items.push_back(native("Toggle full screen", -11, K::F, uint8_t(plat::ModCtrl | C)));
    view.items.push_back(it("Command palette…", K::K, C, [this] { open_palette(); }));
    view.items.push_back(it("Search history", K::F, uint8_t(C | A), [this] { tab = 1; ui.focus = ui.id("history-q"); }, rs != nullptr));
    view.items.push_back(it("Terminal", K::Grave, plat::ModCtrl, [this] { toggle_terminal(); }));
    view.items.push_back(it("Assistant", K::L, C, [this] { agent.open = !agent.open; if (agent.open) agent_load_models(settings.ai_provider); }, ide.open));
    view.items.push_back(sep());
    view.items.push_back(it("Reset zoom", K::Num0, C, [this] { set_zoom(1.0f); }));
    view.items.push_back(it("Zoom in", K::Equals, C, [this] { set_zoom(settings.zoom + 0.1f); }));
    view.items.push_back(it("Zoom out", K::Minus, C, [this] { set_zoom(settings.zoom - 0.1f); }));
    m.push_back(view);
    bool remote = has && !rs->remotes.empty();
    std::string fm = mac ? "Show in Finder" : win ? "Show in Explorer" : "Show in file manager";
    m.push_back({"Repository", {
        it("Push", K::P, C, [this] { push(); }, remote),
        it("Pull", K::P, uint8_t(C | S), [this] { pull(); }, remote && !rs->upstream.empty()),
        it("Fetch", K::T, uint8_t(C | S), [this] { fetch(); }, remote),
        it("Remove…", K::Backspace, C, [this] { remove_d.remove_index = current; dialog = Dialog::ConfirmRemoveRepo; }, has),
        sep(),
        it("View on GitHub", K::G, uint8_t(C | S), [this] { plat::open_url(github_url()); }, has && rs->github.has_value()),
        it(mac ? "Open in Terminal" : win ? "Open in Command Prompt" : "Open in terminal", K::Unknown, 0,
           [this] { plat::open_in_terminal(rs->path, settings.shell_cmd); }, has),
        it(fm, K::F, uint8_t(C | S), [this] { plat::reveal_in_file_manager(rs->path); }, has),
        it("Open in external editor", K::A, uint8_t(C | S), [this] { plat::open_in_editor(rs->path, settings.editor_cmd); }, has),
        it("Edit on GitHub in Tributary", K::Unknown, 0, [this] {
            std::string br = !rs->upstream.empty() ? rs->branch : "";
            open_ide(rs->github->first, rs->github->second, br);
        }, has && rs->github.has_value()),
        sep(),
        it("Create issue on GitHub", K::I, C, [this] { plat::open_url(github_url() + "/issues/new"); }, has && rs->github.has_value()),
        sep(),
        it("Download Git LFS files", K::Unknown, 0, [this] {
            auto path = rs->path;
            auto creds = credentials();
            jobs.run("Download LFS files", [this, path, creds] {
                std::string err;
                size_t n = 0;
                try {
                    auto repo = git::Repository::open(git::git_to_path(path));
                    auto br = repo.current_branch();
                    n = git::lfs::pull(repo, repo.config().get("branch." + br + ".remote").value_or("origin"), creds, {});
                } catch (const std::exception& e) { err = e.what(); }
                jobs.post([this, err, n] {
                    if (!err.empty()) show_error("Couldn't download Git LFS files", err);
                    else toast(n ? "Downloaded " + std::to_string(n) + " Git LFS file" + (n == 1 ? "" : "s") + "." : "All Git LFS files are already here.");
                    refresh(true);
                });
            });
        }, has && !rs->remotes.empty()),
        it("Repository settings…", K::Unknown, 0, [this] { repo_settings_d = {}; dialog = Dialog::RepoSettings; }, has),
    }});
    m.push_back({"Branch", {
        it("New branch…", K::N, uint8_t(C | S), [this] { branch_d = {}; dialog = Dialog::NewBranch; }, has),
        it("Rename…", K::R, uint8_t(C | S), [this] { rename_d = {rs->branch, rs->branch}; dialog = Dialog::RenameBranch; }, br),
        it("Delete…", K::D, uint8_t(C | S), [this] { delete_d = {rs->branch, false}; dialog = Dialog::DeleteBranch; }, br && rs->branch != def),
        sep(),
        it("Discard all changes…", K::Backspace, uint8_t(C | S), [this] {
            discard_d.paths.clear();
            for (auto& f : rs->status.files) discard_d.paths.push_back(f.path);
            dialog = Dialog::ConfirmDiscard;
        }, has && has_changes()),
        it("Stash all changes", K::S, uint8_t(C | S), [this] { stash_all(); }, has && has_changes()),
        sep(),
        it("Update from " + def, K::U, uint8_t(C | S), [this, def] {
            std::string ref = def;
            for (auto& b : rs->branches) if (b.remote && b.name == remote_name() + "/" + def) ref = b.full;
            merge_into_current(ref);
        }, br && rs->branch != def),
        it("Merge into current branch…", K::M, uint8_t(C | S), [this] { merge_d = {}; merge_d.mode = 0; dialog = Dialog::Merge; refresh(true); }, br),
        it("Squash and merge into current branch…", K::H, uint8_t(C | S), [this] { merge_d = {}; merge_d.mode = 1; dialog = Dialog::Merge; refresh(true); }, br),
        it("Rebase current branch…", K::E, uint8_t(C | S), [this] { merge_d = {}; merge_d.mode = 2; dialog = Dialog::Merge; refresh(true); }, br),
        sep(),
        it("Compare on GitHub", K::C, uint8_t(C | S), [this] { plat::open_url(github_url() + "/compare/" + rs->branch); }, br && rs->github.has_value()),
        it("View branch on GitHub", K::B, uint8_t(C | A), [this] { plat::open_url(github_url() + "/tree/" + rs->branch); }, br && rs->github.has_value()),
        it("Preview pull request", K::P, uint8_t(C | A), [this, def] {
            pr_d = {};
            pr_d.base = def;
            dialog = Dialog::PrPreview;
            load_pr_preview();
        }, br && rs->head.has_value()),
        it("Create pull request", K::R, C, [this] { plat::open_url(github_url() + "/compare/" + rs->branch + "?expand=1"); }, br && rs->github.has_value()),
    }});
    AppMenu help{"Help", {
        it("Report issue…", K::Unknown, 0, [] { plat::open_url("https://github.com/p1e2r/tributary/issues/new"); }),
        it("Show user guides", K::Unknown, 0, [] { plat::open_url("https://docs.github.com/en/desktop"); }),
    }};
    help.items.push_back(it("Check for updates…", K::Unknown, 0, [this] { check_for_updates(true); dialog = Dialog::About; }));
    if (!mac) { help.items.push_back(sep()); help.items.push_back(it("About Tributary", K::Unknown, 0, about)); }
    m.push_back(help);
    if (mac) m.insert(m.end() - 1, AppMenu{"Window", {native("Minimize", -12, K::M, C), native("Zoom", -13)}});
    return m;
}

std::string accel_text(plat::Key k, uint8_t mods) {
    if (k == Key::Unknown) return "";
    if (k == Key::Grave) return (mods & plat::ModCtrl ? std::string("Ctrl+") : std::string()) + "`";
    std::string s;
    if (mods & plat::ModCtrl) s += "Ctrl+";
    if (mods & plat::ModAlt) s += "Alt+";
    if (mods & plat::ModShift) s += "Shift+";
    switch (k) {
        case Key::Comma: return s + ",";
        case Key::Equals: return s + "=";
        case Key::Minus: return s + "-";
        case Key::Backspace: return s + "Backspace";
        default: break;
    }
    if (k >= Key::A && k <= Key::Z) return s + char('A' + (int(k) - int(Key::A)));
    if (k >= Key::Num0 && k <= Key::Num9) return s + char('0' + (int(k) - int(Key::Num0)));
    return s;
}

static std::string mac_key_equiv(plat::Key k) {
    switch (k) {
        case Key::Grave: return "`";
        case Key::Comma: return ",";
        case Key::Equals: return "=";
        case Key::Minus: return "-";
        case Key::Backspace: return "\x08";
        default: break;
    }
    if (k >= Key::A && k <= Key::Z) return std::string(1, char('a' + (int(k) - int(Key::A))));
    if (k >= Key::Num0 && k <= Key::Num9) return std::string(1, char('0' + (int(k) - int(Key::Num0))));
    return "";
}

void App::sync_native_menu() {
    if (!win.has_native_menu()) return;
    menus_ = build_menus();
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) { for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ull; } };
    for (auto& mm : menus_)
        for (auto& i : mm.items) { mix(i.label); mix(i.enabled ? "1" : "0"); }
    if (h == native_menu_sig_) return;
    native_menu_sig_ = h;
    std::vector<plat::MenuSpec> specs;
    for (size_t mi = 0; mi < menus_.size(); mi++) {
        plat::MenuSpec sp{menus_[mi].title, {}};
        for (size_t ii = 0; ii < menus_[mi].items.size(); ii++) {
            auto& i = menus_[mi].items[ii];
            plat::MenuItemSpec is;
            is.label = i.label;
            is.separator = i.sep;
            is.enabled = i.enabled;
            is.command = i.native ? i.native : int(mi * 100 + ii + 1);
            is.key = mac_key_equiv(i.key);
            // the Cmd modifier on mac is ModSuper; ModCmd already maps there
            is.mods = i.mods;
            sp.items.push_back(is);
        }
        specs.push_back(sp);
    }
    win.set_menu(specs);
}

void App::run_menu_command(int command) {
    if (command <= 0) return;
    size_t mi = size_t(command - 1) / 100, ii = size_t(command - 1) % 100;
    if (mi < menus_.size() && ii < menus_[mi].items.size()) {
        auto& i = menus_[mi].items[ii];
        if (i.enabled && i.fn) { auto fn = i.fn; fn(); }
    }
}

// Windows/Linux: GitHub Desktop's title-bar menu strip (--win32-title-bar-height).
void App::draw_menu_bar(const Rect& rc) {
    auto& T = ui.theme;
    menus_ = build_menus();
    r.rect(rc, T.dark ? T.bg_toolbar : Color::hex(0x24292e));
    float x = rc.x + 8;
    // app icon
    r.icon("tributary-mark", x + 2, rc.y + (rc.h - 16) / 2, 16, T.toolbar_text_dim);
    x += 30;
    auto pid = ui.id("menubar-pane");
    if (!ui.popup_open(pid)) menubar_open_ = -1;
    for (size_t mi = 0; mi < menus_.size(); mi++) {
        auto& mm = menus_[mi];
        float w = ui.text_w(FontId::Regular, 12, mm.title) + 20;
        Rect b{x, rc.y, w, rc.h};
        bool open = menubar_open_ == int(mi);
        bool hov = ui.hovered(b);
        if (open) r.rect(b, T.dark ? Color::hex(0x2f363d) : T.bg);
        else if (hov) r.rect(b, T.toolbar_hover);
        r.draw_text_in(FontId::Regular, 12, b, mm.title, open ? T.text : T.toolbar_text, Align::Center);
        if (ui.clicked(ui.id("menubar-title", int64_t(mi)), b)) {
            if (open) { ui.close_popup(); menubar_open_ = -1; }
            else { ui.open_popup(pid); menubar_open_ = int(mi); }
        } else if (hov && menubar_open_ >= 0 && !open) {
            menubar_open_ = int(mi);  // slide between menus while one is open
        }
        if (open) {
            float pw = 0;
            for (auto& i : mm.items)
                if (!i.sep) pw = std::max(pw, ui.text_w(FontId::Regular, 12, i.label) + ui.text_w(FontId::Regular, 12, accel_text(i.key, i.mods)) + 80);
            pw = std::max(pw, 240.f);
            float ph = 5;
            for (auto& i : mm.items) ph += i.sep ? 11 : 30;
            Rect pr{b.x, rc.bottom(), pw, ph};
            if (ui::begin_popup(ui, pid, pr, T.dark ? Color::hex(0x2f363d) : T.bg, 0.f)) {
                float y = pr.y;
                for (size_t ii = 0; ii < mm.items.size(); ii++) {
                    auto& i = mm.items[ii];
                    if (i.sep) { r.line_h(pr.x, pr.right(), y + 5, T.border); y += 11; continue; }
                    Rect row{pr.x, y, pr.w, 30};
                    bool h2 = i.enabled && ui.hovered(row);
                    if (h2) r.rect(row, T.row_selected_focus);
                    Color fg = h2 ? Color::hex(0xffffff) : i.enabled ? T.text : T.text_muted.with_alpha(0.6f);
                    r.draw_text_in(FontId::Regular, 12, {row.x + 20, row.y, row.w - 40, row.h}, i.label, fg);
                    r.draw_text_in(FontId::Regular, 12, {row.x, row.y, row.w - 10, row.h}, accel_text(i.key, i.mods),
                                   h2 ? Color::hex(0xffffff) : T.text_muted, Align::Right);
                    if (i.enabled && ui.clicked(ui.id("menubar-item", int64_t(mi * 100 + ii)), row)) {
                        ui.close_popup();
                        menubar_open_ = -1;
                        auto fn = i.fn;
                        if (fn) fn();
                        break;
                    }
                    y += 30;
                }
                ui::end_popup(ui);
            }
        }
        x += w;
    }
}

// ================================================================ sidebar
void App::draw_sidebar(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg);
    int n = int(rs->status.files.size());
    int nt = ui::tabs(ui, ui.id("side-tabs"), {rc.x, rc.y, rc.w, 29}, {"Changes", "History"}, tab,
                      {n ? std::to_string(n) : "", ""});
    if (nt != tab) {
        tab = nt;
        if (tab == 1 && !rs->selected_commit && !rs->history.empty()) select_commit(rs->history[0].id);
    }
    Rect body{rc.x, rc.y + 29, rc.w, rc.h - 29};
    // list focus decides between the blue (focused) and gray selection, like Desktop
    if (ui.focus != 0) list_focus_changes = list_focus_history = false;
    if (ui.mouse_pressed[0] && !body.contains(ui.mx, ui.my) && ui.input_layer() == 0) list_focus_changes = list_focus_history = false;
    if (tab == 0) {
        float box_h = 207;
        if (rs->last_commit_for_undo) box_h += 50;
        if (rs->show_coauthors) box_h += 35;
        Rect list{body.x, body.y, body.w, body.h - box_h};
        draw_changes_list(list);
        draw_commit_box({body.x, list.bottom(), body.w, box_h});
    } else {
        // search over the whole history: message, author, email or commit id
        Rect sb{body.x, body.y, body.w, 33};
        r.line_h(sb.x, sb.right(), sb.bottom() - 1, T.border);
        ui::TextFieldOpts so;
        so.placeholder = "Search commits";
        so.icon = "search";
        auto hq = ui.id("history-q");
        bool enter = false;
        std::string before = history_query;
        ui::text_field(ui, hq, {sb.x + 5, sb.y + 4, sb.w - 10, 25}, history_query, so, &enter);
        if (history_query != before) { history_search_at = ui.time; }
        if (ui.has_focus(hq) && ui.key_pressed(Key::Escape)) { ui.take_key(Key::Escape); history_query.clear(); ui.focus = 0; }
        if ((enter || (history_search_at > 0 && ui.time - history_search_at > 0.25)) && git::trim(history_query) != history_query_ran) {
            history_search_at = 0;
            run_history_search();
        }
        if (history_search_at > 0) ui.animate();
        Rect lb{body.x, sb.bottom(), body.w, body.h - sb.h};
        if (git::trim(history_query).empty()) {
            if (!history_query_ran.empty()) { history_query_ran.clear(); history_hits.clear(); }
            draw_history_list(lb);
        } else {
            draw_history_search(lb);
        }
    }
}

static int file_check_state(const FileSel& s) {
    if (!s.included) return 0;
    if (!s.lines) return 1;
    bool any = false, all = true;
    for (bool b : *s.lines) { any |= b; all &= b; }
    return all ? 1 : any ? 2 : 0;
}

void App::draw_changes_list(const Rect& rc) {
    auto& T = ui.theme;
    auto& files = rs->status.files;
    // .changes-list-container .header: 29px, box-alt background, 10px padding
    Rect head{rc.x, rc.y, rc.w, 29};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    int all_state = 0;
    {
        int on = 0, partial = 0;
        for (auto& f : files) {
            int s = file_check_state(rs->sel[f.path]);
            if (s == 1) on++;
            else if (s == 2) partial++;
        }
        all_state = files.empty() ? 0 : (on == int(files.size()) ? 1 : (on || partial) ? 2 : 0);
    }
    if (ui::checkbox(ui, ui.id("sel-all"), {head.x + 10, head.y + 8, 13, 13}, all_state, !files.empty())) {
        bool v = all_state != 1;
        for (auto& f : files) { rs->sel[f.path].included = v; rs->sel[f.path].lines.reset(); }
    }
    std::string label = files.empty() ? "No changed files"
                        : std::to_string(files.size()) + " changed file" + (files.size() == 1 ? "" : "s");
    r.draw_text_in(FontId::Regular, 12, {head.x + 30, head.y, head.w - 40, head.h - 1}, label, T.text, Align::Left);

    bool stash_row = false;
    for (auto& s : rs->stashes) if (s.branch == rs->branch) { stash_row = true; break; }
    Rect list{rc.x, head.bottom(), rc.w, rc.h - head.h - (stash_row ? 40 : 0)};
    const float RH = 29;
    float off = ui::begin_scroll(ui, ui.id("changes-list"), list);
    bool focus_blue = list_focus_changes && !rs->viewing_stash;
    float y = list.y - off;
    int idx = 0;
    // keyboard navigation
    int cur_index = -1;
    for (int i = 0; i < int(files.size()); i++) if (files[size_t(i)].path == rs->selected_file) cur_index = i;
    bool list_focus = ui.focus == 0;
    if (list_focus && tab == 0 && ui.top_layer_is_modal() == false && ui.input_layer() == 0) {
        if (ui.take_key(Key::Down, 0) && cur_index + 1 < int(files.size())) {
            select_file(files[size_t(cur_index + 1)].path);
            ui::scroll_to_visible(ui, ui.id("changes-list"), float(cur_index + 1) * RH, RH, list.h);
        }
        if (ui.take_key(Key::Up, 0) && cur_index > 0) {
            select_file(files[size_t(cur_index - 1)].path);
            ui::scroll_to_visible(ui, ui.id("changes-list"), float(cur_index - 1) * RH, RH, list.h);
        }
        if (ui.take_key(Key::Space, 0) && cur_index >= 0) {
            auto& s0 = rs->sel[rs->selected_file];
            bool v = file_check_state(s0) != 1;
            std::set<std::string> targets = rs->files_sel;
            targets.insert(rs->selected_file);
            for (auto& p : targets) { rs->sel[p].included = v; rs->sel[p].lines.reset(); }
        }
        if (ui.take_key(Key::A, plat::ModCmd) && !files.empty()) {
            for (auto& f : files) rs->files_sel.insert(f.path);
        }
    }
    for (auto& f : files) {
        Rect row{list.x, y, list.w, RH};
        if (row.bottom() >= list.y && row.y <= list.bottom()) {
            bool sel = (f.path == rs->selected_file || rs->files_sel.count(f.path)) && !rs->viewing_stash;
            bool rc_click = false;
            bool blue = sel && focus_blue;
            if (sel) r.rect(row, blue ? T.row_selected_focus : T.row_selected);
            else if (ui.hovered(row)) r.rect(row, T.row_hover);
            r.line_h(row.x, row.right(), row.bottom() - 1, T.border);
            Rect cb{row.x + 10, row.y + 8, 13, 13};
            if (blue) r.rect(cb.inset(-1), Color::hex(0xffffff), 4);  // keep the checkbox visible on the blue row
            auto& s = rs->sel[f.path];
            int st = file_check_state(s);
            if (ui::checkbox(ui, ui.id("cb", idx), cb, st)) {
                s.included = st != 1;
                s.lines.reset();
            } else if (ui::row_click(ui, ui.id("file", idx), {row.x + 30, row.y, row.w - 30, row.h}, nullptr, &rc_click)) {
                rs->viewing_stash = false;
                list_focus_changes = true;
                ui.focus = 0;
                if (ui.mods & plat::ModCmd) {
                    // toggle in multi-selection
                    if (rs->files_sel.empty() && !rs->selected_file.empty()) rs->files_sel.insert(rs->selected_file);
                    if (rs->files_sel.count(f.path)) rs->files_sel.erase(f.path); else rs->files_sel.insert(f.path);
                    rs->files_anchor = idx;
                } else if ((ui.mods & plat::ModShift) && cur_index >= 0) {
                    int a = rs->files_anchor >= 0 ? rs->files_anchor : cur_index;
                    rs->files_sel.clear();
                    for (int k = std::min(a, idx); k <= std::max(a, idx); k++) rs->files_sel.insert(files[size_t(k)].path);
                } else {
                    rs->files_sel.clear();
                    rs->files_anchor = idx;
                    select_file(f.path);
                }
                if (rs->files_sel.size() == 1) { select_file(*rs->files_sel.begin()); rs->files_sel.clear(); }
            }
            if (rc_click) list_focus_changes = true;
            Color fg = blue ? T.text_inverse : T.text, fg2 = blue ? T.text_inverse : T.text_muted;
            auto [dir, name] = split_path(f.path);
            float iw = 30;
            Rect tr{row.x + 30, row.y, row.w - 30 - iw, row.h - 1};
            std::string display = f.old_path.empty() ? f.path : f.old_path + " → " + f.path;
            float nw = ui.text_w(FontId::Regular, 12, name);
            if (!f.old_path.empty()) {
                r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, display, tr.w), fg);
            } else if (nw + 20 >= tr.w || dir.empty()) {
                r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, f.path, tr.w), fg);
            } else {
                std::string d = r.ellipsize_middle(FontId::Regular, 12, dir, tr.w - nw);
                float dw = r.draw_text_in(FontId::Regular, 12, tr, d, fg2);
                r.draw_text_in(FontId::Regular, 12, {tr.x + dw, tr.y, tr.w - dw, tr.h}, name, fg);
            }
            r.icon(change_icon(f.change), row.right() - 26, row.y + 6, 16, blue ? T.text_inverse : change_color(T, f.change));
            if (rc_click && rs->files_sel.size() > 1 && rs->files_sel.count(f.path)) {
                std::vector<std::string> ps(rs->files_sel.begin(), rs->files_sel.end());
                std::string joined;
                for (auto& p : ps) joined += rs->path + "/" + p + "\n";
                open_context({
                    {"Discard " + std::to_string(ps.size()) + " selected changes…", [this, ps] { discard_d.paths = ps; dialog = Dialog::ConfirmDiscard; }},
                    {"Include in commit", [this, ps] { for (auto& p : ps) { rs->sel[p].included = true; rs->sel[p].lines.reset(); } }},
                    {"Exclude from commit", [this, ps] { for (auto& p : ps) { rs->sel[p].included = false; rs->sel[p].lines.reset(); } }},
                    {"", {}, true, true},
                    {"Copy file paths", [this, joined] { ui.set_clipboard(joined); }},
                });
                rc_click = false;
            }
            if (rc_click) {
                std::string p = f.path;
                auto ext = fs::path(git::git_to_path(p)).extension().string();
                std::string full = rs->path + "/" + p;
                std::vector<MenuEntry> items = {
                    {"Discard changes…", [this, p] {
                         discard_d.paths = {p};
                         if (settings.confirm_discard) dialog = Dialog::ConfirmDiscard; else discard({p});
                     }},
                    {"Discard all changes…", [this] {
                         discard_d.paths.clear();
                         for (auto& ff : rs->status.files) discard_d.paths.push_back(ff.path);
                         dialog = Dialog::ConfirmDiscard;
                     }},
                    {"", {}, true, true},
                    {"Ignore file (add to .gitignore)", [this, p] {
                         auto path = rs->path;
                         jobs.run("Ignore", [this, path, p] {
                             try { git::Repository::open(git::git_to_path(path)).add_to_gitignore("/" + p); } catch (...) {}
                             jobs.post([this] { refresh(true); });
                         });
                     }, f.change == git::Change::Untracked},
                };
                if (!ext.empty())
                    items.push_back({"Track *" + ext + " files with Git LFS", [this, ext] {
                                         auto path = rs->path;
                                         jobs.run("Track with Git LFS", [this, path, ext] {
                                             try { auto repo = git::Repository::open(git::git_to_path(path)); git::lfs::track(repo, "*" + ext); } catch (...) {}
                                             jobs.post([this] { refresh(true); toast("Tracked with Git LFS. Commit .gitattributes along with the files."); });
                                         });
                                     }});
                if (!ext.empty())
                    items.push_back({"Ignore all *" + ext + " files", [this, ext] {
                                         auto path = rs->path;
                                         jobs.run("Ignore", [this, path, ext] {
                                             try { git::Repository::open(git::git_to_path(path)).add_to_gitignore("*" + ext); } catch (...) {}
                                             jobs.post([this] { refresh(true); });
                                         });
                                     }});
                items.push_back({"", {}, true, true});
                if (f.change != git::Change::Untracked && f.change != git::Change::Added && rs->head)
                    items.push_back({"Blame (who changed each line)", [this, p] { open_blame(p, *rs->head); }});
                items.push_back({"Copy file path", [this, full] { ui.set_clipboard(full); }});
                items.push_back({"Copy relative file path", [this, p] { ui.set_clipboard(p); }});
                items.push_back({"Show in file manager", [full] { plat::reveal_in_file_manager(fs::path(git::git_to_path(full)).parent_path().string()); }});
                items.push_back({"Open in editor", [this, full] { plat::open_in_editor(full, settings.editor_cmd); }});
                open_context(items);
            }
        }
        y += RH;
        idx++;
    }
    ui::end_scroll(ui, ui.id("changes-list"), list, y + off - list.y);
    if (stash_row) {
        Rect sr{rc.x, list.bottom(), rc.w, 40};
        r.line_h(sr.x, sr.right(), sr.y, T.border);
        if (ui::selectable(ui, ui.id("stash-row"), sr.inset(0, 1), rs->viewing_stash)) {
            if (rs->viewing_stash) rs->viewing_stash = false;
            else load_stash_view();
        }
        r.icon("stack", sr.x + 14, sr.y + 12, 16, T.text_muted);
        r.draw_text_in(FontId::Bold, 12, {sr.x + 40, sr.y, sr.w - 60, sr.h}, "Stashed changes", T.text);
        r.icon("chevron-right", sr.right() - 28, sr.y + 12, 16, T.text_muted);
    }
}

void App::draw_commit_box(const Rect& rc) {
    // changes/_commit-message.scss: box-alt background, 10px padding, 25px avatar + summary,
    // description box with an action bar, full-width commit button
    auto& T = ui.theme;
    r.rect(rc, T.bg_subtle);
    r.line_h(rc.x, rc.right(), rc.y, T.border);
    float y = rc.y + 10;
    Rect av{rc.x + 10, y, 25, 25};
    std::string who = account.valid() ? (account.name.empty() ? account.login : account.name) : git_user_name;
    if (who.empty()) who = "?";
    if (ui.hovered(av)) ui.tooltip(ui.id("commit-avatar"), av, "Committing as " + who + (git_user_email.empty() ? "" : " <" + git_user_email + ">"));
    draw_avatar(av, who, git_user_email);
    bool merging = rs->status.merging;
    if (merging && rs->summary.empty()) rs->summary = git::trim(rs->status.merge_msg.substr(0, rs->status.merge_msg.find('\n')));
    ui::TextFieldOpts so;
    so.placeholder = merging ? "Merge commit message" : rs->amending ? "Amended commit summary" : "Summary (required)";
    bool submit = false;
    summary_id = ui.id("summary");
    ui::text_field(ui, ui.id("summary"), {rc.x + 40, y, rc.w - 50 - 30, 25}, rs->summary, so, &submit);
    // write the message with the assistant's model, from the diff of the selected files
    {
        Rect gb{rc.right() - 10 - 25, y, 25, 25};
        if (ai_msg_running) {
            if (ui::icon_button(ui, ui.id("ai-msg-stop"), gb, "x", "Stop writing") && ai_msg_cancel) ai_msg_cancel->store(true);
            ui::spinner(ui, gb.x - 12, gb.y + 12, 5, T.accent);
        } else if (ui::icon_button(ui, ui.id("ai-msg"), gb, "sparkle-fill", "Write a commit message from the changes (AI)")) {
            generate_commit_message();
        }
    }
    // .length-hint
    if (rs->summary.size() > 72) {
        Rect lh{rc.right() - 60, y + 2, 16, 21};
        r.icon("light-bulb", lh.x + 2, lh.y + 4, 12, T.attention);
        if (ui.hovered(lh)) ui.tooltip(ui.id("summary-long"), lh,
                   "Great commit summaries contain fewer than 50 characters. Place extra information in the description field.");
    }
    // commit attribution warning badge on the avatar (.warning-badge)
    {
        auto* acc = account_for_url(rs->remotes.empty() ? "" : rs->remotes[0].url);
        if (acc && !git_user_email.empty() && !acc->emails.empty() &&
            std::find(acc->emails.begin(), acc->emails.end(), git_user_email) == acc->emails.end()) {
            Rect wr{av.right() - 11, av.y - 6, 18, 18};
            r.rect(wr, T.dark ? T.bg : Color::hex(0xfafbfc), 9, 1, T.dark ? T.border_contrast : Color::hex(0xd1d5da));
            r.icon("alert", wr.x + 4, wr.y + 4, 10, T.attention);
            if (ui.hovered(av.inset(-2)))
                ui.tooltip(ui.id("attrib"), av, "Commits won't be attributed to @" + acc->login + ": " + git_user_email +
                                                    " isn't one of your GitHub emails. Click to fix.");
            if (ui.clicked(ui.id("attrib-click"), av)) { settings_d = {}; settings_d.tab = 2; dialog = Dialog::Settings; }
        }
    }
    y += 35;
    // description field with its action bar inside one bordered box
    Rect dbox{rc.x + 10, y, rc.w - 20, 117};
    bool dfocus = ui.has_focus(ui.id("description"));
    if (dfocus) r.rect(dbox.inset(-1), Color{3 / 255.f, 102 / 255.f, 214 / 255.f, 0.25f}, 7);
    r.rect(dbox, T.input_bg, 6, 1, dfocus ? T.focus_ring : T.input_border);
    ui::TextFieldOpts dopt;
    dopt.multiline = true;
    dopt.placeholder = "Description";
    dopt.borderless = true;
    ui::text_field(ui, ui.id("description"), {dbox.x + 1, dbox.y + 1, dbox.w - 2, 80}, rs->description, dopt);
    Rect bar{dbox.x + 1, dbox.y + 80, dbox.w - 2, 36};
    Rect cob{bar.x + 10, bar.y + 10, 18, 17};
    bool coh = ui.hovered(cob.inset(-3));
    r.icon("person-add", cob.x + 1, cob.y + 1, 16, rs->show_coauthors ? T.accent_emphasis : coh ? T.text : T.text_muted);
    if (coh) { ui.set_cursor(plat::Cursor::Hand); ui.tooltip(ui.id("coauthor-tip"), cob, rs->show_coauthors ? "Remove co-authors" : "Add co-authors"); }
    if (ui.clicked(ui.id("coauthor-btn"), cob.inset(-3))) rs->show_coauthors = !rs->show_coauthors;
    y += 117;
    if (rs->show_coauthors) {
        ui::TextFieldOpts co;
        co.placeholder = "@username or email";
        ui::text_field(ui, ui.id("coauthors"), {rc.x + 10, y + 10, rc.w - 20, 25}, rs->coauthors, co);
        y += 35;
    }
    y += 10;
    int count = 0;
    for (auto& f : rs->status.files) if (file_check_state(rs->sel[f.path])) count++;
    std::string bname = rs->branch.empty() ? "HEAD" : rs->branch;
    std::string label = merging ? "Commit merge" : rs->amending ? "Amend last commit" :
                        count > 1 && count == int(rs->status.files.size()) ? "Commit " + std::to_string(count) + " files to " + bname :
                        "Commit to " + bname;
    // the branch name is bold in Desktop's commit button
    std::string bold_part = (!merging && !rs->amending) ? bname : "";
    bool can = !git::trim(rs->summary).empty() && (count > 0 || rs->amending || merging) && !(merging && rs->status.has_conflicts() &&
                                                                                    std::any_of(rs->status.files.begin(), rs->status.files.end(),
                                                                                                [](auto& f) { return f.conflicted && f.has_markers; }));
    Rect btn{rc.x + 10, y, rc.w - 20, 25};
    bool cmd_enter = ui.key_pressed(Key::Enter, plat::ModCmd) && tab == 0 && ui.input_layer() == 0;
    bool commit_clicked = ui::button(ui, ui.id("commit-btn"), btn, bold_part.empty() ? label : "", ui::BtnStyle::Primary, can && !jobs.busy());
    if (!bold_part.empty()) {
        std::string pre = label.substr(0, label.size() - bold_part.size());
        float w1 = ui.text_w(FontId::Regular, 12, pre), w2 = ui.text_w(FontId::Bold, 12, bold_part);
        float avail = btn.w - 20;
        std::string bp = w1 + w2 > avail ? r.ellipsize(FontId::Bold, 12, bold_part, std::max(20.f, avail - w1)) : bold_part;
        w2 = ui.text_w(FontId::Bold, 12, bp);
        float tx = std::round(btn.x + (btn.w - w1 - w2) / 2);
        Color fg = Color::hex(0xffffff).with_alpha(can && !jobs.busy() ? 1.f : 0.6f);
        r.draw_text_in(FontId::Regular, 12, {tx, btn.y, w1 + 1, btn.h}, pre, fg);
        r.draw_text_in(FontId::Bold, 12, {tx + w1, btn.y, w2 + 1, btn.h}, bp, fg);
    }
    if (commit_clicked || (cmd_enter && can)) {
        ui.take_key(Key::Enter, plat::ModCmd);
        commit();
    }
    if (ui.hovered(btn) && ui.mouse_pressed[1] && rs->head) {
        open_context({{rs->amending ? "Stop amending" : "Amend last commit", [this] {
                           rs->amending = !rs->amending;
                           if (rs->amending && !rs->history.empty() && rs->summary.empty()) {
                               rs->summary = rs->history[0].commit.summary();
                               rs->description = rs->history[0].commit.body();
                           }
                       }}});
    }
    y += 35;
    if (rs->last_commit_for_undo) {
        // changes/_undo-commit.scss
        Rect u{rc.x, y, rc.w, 50};
        r.rect(u, T.bg_subtle);
        r.line_h(u.x, u.right(), u.y, T.border);
        r.draw_text_in(FontId::Regular, 11, {u.x + 10, u.y + 7, u.w - 100, 16}, "Committed just now", T.text_muted);
        r.draw_text_in(FontId::Regular, 12, {u.x + 10, u.y + 24, u.w - 100, 18}, r.ellipsize(FontId::Regular, 12, rs->last_commit_summary, u.w - 100), T.text);
        if (ui::button(ui, ui.id("undo"), {u.right() - 70, u.y + 12, 60, 25}, "Undo", ui::BtnStyle::Default, !jobs.busy() && rs->ahead > 0 || rs->upstream.empty()))
            undo_commit();
    }
    (void)submit;
}

// ================================================================ history
void App::draw_history_list(const Rect& rc) {
    auto& T = ui.theme;
    auto& h = rs->history;
    const float RH = 50;
    Id lid = ui.id("history-list");
    float off = ui::begin_scroll(ui, lid, rc);
    float y = rc.y - off;
    int first = std::max(0, int(off / RH));
    y += float(first) * RH;
    int cur = -1;
    for (int i = 0; i < int(h.size()); i++) if (rs->selected_commit == h[size_t(i)].id) cur = i;
    if (ui.focus == 0 && tab == 1 && ui.input_layer() == 0) {
        if (ui.take_key(Key::Down, 0) && cur + 1 < int(h.size())) { rs->history_sel.clear(); select_commit(h[size_t(cur + 1)].id); ui::scroll_to_visible(ui, lid, float(cur + 1) * RH, RH, rc.h); }
        if (ui.take_key(Key::Up, 0) && cur > 0) { rs->history_sel.clear(); select_commit(h[size_t(cur - 1)].id); ui::scroll_to_visible(ui, lid, float(cur - 1) * RH, RH, rc.h); }
    }
    // tags by commit
    std::map<ObjectId, std::vector<std::string>> tags_at;
    for (auto& [name, id] : rs->tags) {
        ObjectId target = id;
        try { if (auto o = git::Repository::open(git::git_to_path(rs->path)).odb().try_read(id); o && o->type == git::ObjType::Tag) target = git::Tag::parse(o->data).object; } catch (...) {}
        tags_at[target].push_back(name);
    }
    // ---- drag & drop state
    if (hdrag.pending && ui.mouse_down[0] && (std::abs(ui.mx - hdrag.start_x) > 6 || std::abs(ui.my - hdrag.start_y) > 6)) {
        hdrag.active = true;
        hdrag.pending = false;
    }
    if (hdrag.active) {
        hdrag.drop_index = -1;
        hdrag.squash_index = -1;
        if (rc.contains(ui.mx, ui.my)) {
            float rel = ui.my - rc.y + off;
            int idx = int(rel / RH);
            float frac = rel / RH - float(idx);
            if (idx >= 0 && idx < int(h.size())) {
                if (frac > 0.3f && frac < 0.7f) hdrag.squash_index = idx;
                else hdrag.drop_index = frac <= 0.3f ? idx : idx + 1;
            } else if (idx >= int(h.size())) hdrag.drop_index = int(h.size());
            // auto-scroll near the edges
            if (ui.my < rc.y + 30) { ui.scroll_state(lid).offset -= 8; ui.animate(); }
            if (ui.my > rc.bottom() - 30) { ui.scroll_state(lid).offset += 8; ui.animate(); }
        }
        if (!ui.mouse_down[0]) {
            // drop
            auto commits = hdrag.commits;
            bool pushed = false;
            for (auto& c : commits) pushed |= commit_is_pushed(c);
            if (!hdrag.branch_target.empty()) {
                std::string target = hdrag.branch_target;
                ui.close_all_popups();
                cherry_pick(commits, target);
            } else if (hdrag.squash_index >= 0) {
                auto into = h[size_t(hdrag.squash_index)].id;
                if (std::find(commits.begin(), commits.end(), into) == commits.end()) {
                    pushed |= commit_is_pushed(into);
                    std::vector<ObjectId> all = commits;
                    all.push_back(into);
                    squash_d = {};
                    squash_d.commits = all;
                    squash_d.into = into;
                    // combined message: target first, then the others
                    for (auto& c : rs->history) if (c.id == into) { squash_d.summary = c.commit.summary(); squash_d.description = c.commit.body(); }
                    for (auto& c : rs->history)
                        if (std::find(commits.begin(), commits.end(), c.id) != commits.end()) {
                            squash_d.description += (squash_d.description.empty() ? "" : "\n\n") + c.commit.summary();
                            if (!c.commit.body().empty()) squash_d.description += "\n\n" + c.commit.body();
                        }
                    confirm_rewrite(pushed, "squash", [this] { dialog = Dialog::Squash; });
                }
            } else if (hdrag.drop_index >= 0) {
                // The moved commits land just below the row above the insertion line
                // (i.e. directly older than it). Zero target = top of the branch.
                int k = hdrag.drop_index - 1;
                while (k >= 0 && std::find(commits.begin(), commits.end(), h[size_t(k)].id) != commits.end()) k--;
                ObjectId target = k >= 0 ? h[size_t(k)].id : ObjectId{};
                int lowest = -1;
                for (int q = 0; q < int(h.size()); q++)
                    if (std::find(commits.begin(), commits.end(), h[size_t(q)].id) != commits.end()) { lowest = std::max(lowest, q); pushed |= commit_is_pushed(h[size_t(q)].id); }
                if (k >= 0) pushed |= commit_is_pushed(target);
                bool noop = commits.size() == 1 && (hdrag.drop_index == lowest || hdrag.drop_index == lowest + 1);
                if (!noop) confirm_rewrite(pushed, "reorder", [this, commits, target] { reorder_commits(commits, target); });
            }
            hdrag = {};
        }
    } else if (!ui.mouse_down[0]) hdrag.pending = false;

    for (int i = first; i < int(h.size()); i++) {
        Rect row{rc.x, y, rc.w, RH};
        if (row.y > rc.bottom()) break;
        auto& c = h[size_t(i)];
        bool sel = rs->selected_commit == c.id || rs->history_sel.count(c.id);
        bool rc_click = false;
        bool dragged = hdrag.active && std::find(hdrag.commits.begin(), hdrag.commits.end(), c.id) != hdrag.commits.end();
        bool blue = sel && list_focus_history && !hdrag.active;
        if (sel) r.rect(row, blue ? T.row_selected_focus : T.row_selected);
        else if (ui.hovered(row) && !hdrag.active) r.rect(row, T.row_hover);
        if (!hdrag.active && ui::row_click(ui, ui.id("hist", i), row, nullptr, &rc_click)) {
            list_focus_history = true;
            ui.focus = 0;
            if (ui.mods & plat::ModCmd) {
                if (rs->history_sel.empty() && rs->selected_commit) rs->history_sel.insert(*rs->selected_commit);
                if (rs->history_sel.count(c.id)) rs->history_sel.erase(c.id); else rs->history_sel.insert(c.id);
            } else if ((ui.mods & plat::ModShift) && cur >= 0) {
                rs->history_sel.clear();
                for (int k = std::min(cur, i); k <= std::max(cur, i); k++) rs->history_sel.insert(h[size_t(k)].id);
            } else {
                rs->history_sel.clear();
                select_commit(c.id);
            }
        } else if (hdrag.active && sel) r.rect(row, T.row_selected);
        // start a drag
        if (!hdrag.active && ui.hovered(row) && ui.mouse_pressed[0] && !(ui.mods & (plat::ModCmd | plat::ModShift))) {
            hdrag.pending = true;
            hdrag.start_x = ui.mx;
            hdrag.start_y = ui.my;
            hdrag.commits = history_selection_or(c.id);
            if (!rs->history_sel.count(c.id)) hdrag.commits = {c.id};
        }
        if (hdrag.active && hdrag.squash_index == i) r.rect(row.inset(2), T.accent_emphasis.with_alpha(0.2f), 6, 1.5f, T.accent_emphasis);
        if (rc_click) list_focus_history = true;
        // history/_commit-list.scss: padding 0 15 0 10, semibold summary, 16px avatar + byline,
        // indicators (tags, unpushed) as 16px badges on the right
        r.line_h(row.x, row.right(), row.bottom() - 1, T.border);
        bool unpushed = i < rs->ahead || rs->upstream.empty();
        auto& tg = tags_at[c.id];
        Color fg = blue ? T.text_inverse : dragged ? T.text_muted : T.text;
        Color fg2 = blue ? T.text_inverse : T.text_muted;
        Color badge = blue ? Color::hex(0xffffff) : sel ? T.badge_selected_bg : T.badge_bg;
        Color badge_fg = blue ? Color::hex(0x24292e) : T.dark ? T.text : Color::hex(0x2f363d);
        float right = row.right() - 15;
        float ind_w = 0;
        bool show_unpushed = i < rs->ahead && !rs->upstream.empty();
        if (show_unpushed) {
            std::string n = std::to_string(rs->ahead);
            (void)n;
            ind_w += 26 + 5;
        }
        for (auto& t : tg) ind_w += std::min(ui.text_w(FontId::Regular, 12, t) + 10, (row.w - 25) * 0.25f);
        ind_w = std::min(ind_w, (row.w - 25) * 0.5f);
        float iy = row.y + 17;
        float ix = right - ind_w;
        for (auto& t : tg) {
            float w = std::min(ui.text_w(FontId::Regular, 12, t) + 10, (row.w - 25) * 0.25f);
            Rect tb{ix, iy, w, 16};
            r.rect(tb, badge, 6);
            r.draw_text_in(FontId::Regular, 12, {tb.x + 5, tb.y, w - 10, 16}, r.ellipsize(FontId::Regular, 12, t, w - 10), badge_fg);
            ix += w;
        }
        if (show_unpushed) {
            Rect ub{right - 26, iy, 26, 16};
            r.rect(ub, badge, 8);
            r.icon("arrow-up", ub.x + 5, ub.y, 16, badge_fg);
            if (ui.hovered(ub)) ui.tooltip(ui.id("unpushed", i), ub, "This commit hasn't been pushed to the remote repository yet");
        }
        float tw = row.w - 25 - (ind_w ? ind_w + 10 : 0);
        std::string summary = c.commit.summary();
        r.draw_text_in(FontId::Bold, 12, {row.x + 10, row.y + 8, tw, 18},
                       summary.empty() ? "Empty commit message" : r.ellipsize(FontId::Bold, 12, summary, tw),
                       summary.empty() ? fg2 : fg);
        draw_avatar({row.x + 10, row.y + 27, 16, 16}, c.commit.author.name, c.commit.author.email);
        std::string who = c.commit.author.name;
        int co = 0;
        for (size_t p = c.commit.message.find("Co-authored-by:"); p != std::string::npos; p = c.commit.message.find("Co-authored-by:", p + 1)) co++;
        if (co) who += " and " + std::to_string(co) + " other" + (co == 1 ? "" : "s");
        std::string by = who + " \u2022 " + git::format_relative_time(c.commit.author.time, git::unix_now());
        r.draw_text_in(FontId::Regular, 12, {row.x + 31, row.y + 26, row.w - 46, 18}, r.ellipsize(FontId::Regular, 12, by, row.w - 46), fg2);
        if (rc_click) {
            auto id = c.id;
            bool is_head = i == 0;
            auto multi = history_selection_or(id);
            bool linear = c.commit.parents.size() <= 1;
            bool pushed = commit_is_pushed(id);
            std::vector<MenuEntry> items;
            if (is_head && unpushed) items.push_back({"Undo commit…", [this] { undo_commit(); }});
            if (is_head && unpushed) items.push_back({"Amend commit…", [this] {
                tab = 0;
                rs->amending = true;
                rs->summary = rs->history[0].commit.summary();
                rs->description = rs->history[0].commit.body();
            }});
            items.push_back({"Reword commit…", [this, id, pushed] {
                reword_d = {id, "", ""};
                for (auto& cc : rs->history) if (cc.id == id) { reword_d.summary = cc.commit.summary(); reword_d.description = cc.commit.body(); }
                confirm_rewrite(pushed, "reword", [this] { dialog = Dialog::Reword; });
            }, linear && !has_changes() && !rs->branch.empty()});
            if (i + 1 < int(h.size()))
                items.push_back({"Squash into previous commit…", [this, id, i, pushed] {
                    auto into = rs->history[size_t(i + 1)].id;
                    squash_d = {};
                    squash_d.commits = {id, into};
                    squash_d.into = into;
                    squash_d.summary = rs->history[size_t(i + 1)].commit.summary();
                    squash_d.description = rs->history[size_t(i + 1)].commit.body();
                    squash_d.description += (squash_d.description.empty() ? "" : "\n\n") + rs->history[size_t(i)].commit.summary();
                    confirm_rewrite(pushed || commit_is_pushed(into), "squash", [this] { dialog = Dialog::Squash; });
                }, linear && !has_changes() && !rs->branch.empty()});
            items.push_back({multi.size() > 1 ? "Cherry-pick " + std::to_string(multi.size()) + " commits…" : "Cherry-pick commit…", [this, multi] {
                cherry_d = {};
                cherry_d.commits = multi;
                dialog = Dialog::CherryPick;
            }, !has_changes()});
            items.push_back({"Revert changes in commit", [this, id] { revert_commit(id); }, !has_changes()});
            items.push_back({"", {}, true, true});
            items.push_back({"Create branch from commit", [this, id] { branch_d = {}; branch_d.from_commit = id; dialog = Dialog::NewBranch; }});
            items.push_back({"Create tag…", [this, id] { tag_d = {"", id}; dialog = Dialog::CreateTag; }});
            for (auto& t : tg) {
                std::string tn = t;
                items.push_back({"Delete tag " + tn + "…", [this, tn] { deltag_d = {tn, false}; dialog = Dialog::DeleteTag; }});
            }
            items.push_back({"Reset to commit…", [this, id] { reset_d.commit = id; dialog = Dialog::ResetToCommit; }, !is_head && !rs->branch.empty()});
            items.push_back({"Check out commit", [this, id] {
                auto cid = id;
                jobs.run("Checkout", [this, cid, path = rs->path] {
                    std::string err;
                    try { git::Repository::open(git::git_to_path(path)).checkout_commit(cid); } catch (const std::exception& e) { err = e.what(); }
                    jobs.post([this, err] { if (!err.empty()) show_error("Checkout failed", err); refresh(true); load_history(false); });
                });
            }});
            items.push_back({"", {}, true, true});
            items.push_back({"Copy SHA", [this, id] { ui.set_clipboard(id.hex()); }});
            items.push_back({"Copy tag", [this, tg] { if (!tg.empty()) ui.set_clipboard(tg[0]); }, !tg.empty()});
            if (rs->github) items.push_back({"View on GitHub", [this, id] { plat::open_url(github_url() + "/commit/" + id.hex()); }});
            open_context(items);
        }
        y += RH;
    }
    float total = float(h.size()) * RH;
    // insertion indicator
    if (hdrag.active && hdrag.drop_index >= 0) {
        float iy = rc.y - off + float(hdrag.drop_index) * RH;
        r.rect({rc.x + 6, iy - 1.5f, rc.w - 12, 3}, T.accent_emphasis, 1.5f);
        r.rect({rc.x + 2, iy - 4, 8, 8}, T.accent_emphasis, 4);
    }
    if (rs->history_loading) {
        ui::spinner(ui, rc.x + rc.w / 2, rc.y - off + total + 20, 6, T.text_muted);
        total += 40;
    }
    ui::end_scroll(ui, lid, rc, total);
    if (!rs->history_complete && !rs->history_loading && off + rc.h > total - 400 && !h.empty()) load_history(true);
    if (h.empty() && !rs->history_loading)
        r.draw_text_in(FontId::Regular, 12, {rc.x, rc.y + 40, rc.w, 24}, "No commits yet", T.text_muted, Align::Center);
    // drag ghost
    if (hdrag.active) {
        ui.set_layer(92);
        std::string label = hdrag.commits.size() == 1 ? "1 commit" : std::to_string(hdrag.commits.size()) + " commits";
        if (!hdrag.branch_target.empty()) label = "Cherry-pick onto " + hdrag.branch_target;
        else if (hdrag.squash_index >= 0) label = "Squash into this commit";
        else if (hdrag.drop_index >= 0) label = "Move here";
        float w = ui.text_w(FontId::Bold, 12, label) + 40;
        Rect g{ui.mx + 14, ui.my + 10, w, 28};
        r.shadow({g.x, g.y + 2, g.w, g.h}, 6, 8, T.shadow);
        r.rect(g, T.accent_emphasis, 6);
        r.icon("git-commit", g.x + 8, g.y + 6, 16, Color::hex(0xffffff));
        r.draw_text_in(FontId::Bold, 12, {g.x + 30, g.y, g.w - 34, g.h}, label, Color::hex(0xffffff));
        ui.set_layer(0);
        ui.request_redraw();
    }
}

// ================================================================ main area
void App::draw_conflict_banner(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.attention_subtle);
    r.line_h(rc.x, rc.right(), rc.bottom() - 1, T.border);
    int n = 0;
    for (auto& f : rs->status.files) if (f.conflicted && f.has_markers) n++;
    r.icon("alert", rc.x + 14, rc.y + (rc.h - 16) / 2, 16, T.attention);
    std::string msg = n ? "Resolve " + std::to_string(n) + " conflicted file" + (n == 1 ? "" : "s") + " to finish merging."
                        : "All conflicts resolved. Commit the merge to finish.";
    r.draw_text_in(FontId::Regular, 12, {rc.x + 40, rc.y, rc.w - 260, rc.h}, msg, T.text);
    if (ui::button(ui, ui.id("conf-view"), {rc.right() - 220, rc.y + 6, 110, rc.h - 12}, "View conflicts")) dialog = Dialog::Conflicts;
    if (ui::button(ui, ui.id("conf-abort"), {rc.right() - 104, rc.y + 6, 92, rc.h - 12}, "Abort merge", ui::BtnStyle::Danger)) {
        abort_merge();
    }
}

void App::draw_rewrite_banner(const Rect& rc) {
    auto& T = ui.theme;
    auto& st = *rs->status.rewrite;
    r.rect(rc, T.attention_subtle);
    r.line_h(rc.x, rc.right(), rc.bottom() - 1, T.border);
    int n = 0;
    for (auto& f : rs->status.files) if (f.conflicted && f.has_markers) n++;
    r.icon("alert", rc.x + 14, rc.y + (rc.h - 16) / 2, 16, T.attention);
    std::string step = std::to_string(std::min(st.next + 1, st.steps.size())) + " of " + std::to_string(st.steps.size());
    std::string msg = st.label + " paused at commit " + step + ": " +
                      (n ? "resolve " + std::to_string(n) + " conflicted file" + (n == 1 ? "" : "s") + ", then continue." : "conflicts resolved — continue to finish.");
    r.draw_text_in(FontId::Regular, 12, {rc.x + 40, rc.y, rc.w - 280, rc.h}, msg, T.text);
    if (ui::button(ui, ui.id("rw-continue"), {rc.right() - 230, rc.y + 6, 110, rc.h - 12}, "Continue", ui::BtnStyle::Primary, n == 0 && !jobs.busy()))
        rewrite_continue();
    if (ui::button(ui, ui.id("rw-abort"), {rc.right() - 112, rc.y + 6, 100, rc.h - 12}, "Abort", ui::BtnStyle::Danger, !jobs.busy())) rewrite_abort();
}

void App::draw_multi_selection(const Rect& rc) {
    // MultipleSelection: illustration + count, like Desktop
    auto& T = ui.theme;
    size_t n = rs->files_sel.size();
    float iw = 288, ih = 147;
    float cy = std::round(rc.y + rc.h / 2 - 110);
    illustration("multiple-files-selected", {std::round(rc.x + (rc.w - iw) / 2), cy, iw, ih});
    r.draw_text_in(FontId::Regular, 12, {rc.x, cy + ih + 16, rc.w, 18}, std::to_string(n) + " files selected", T.text, Align::Center);
    std::vector<std::string> ps(rs->files_sel.begin(), rs->files_sel.end());
    std::string lbl = "Discard " + std::to_string(n) + " selected changes";
    float bw = std::max(120.f, ui.text_w(FontId::Regular, 12, lbl) + 20);
    if (ui::button(ui, ui.id("ms-discard"), {std::round(rc.x + (rc.w - bw) / 2), cy + ih + 46, bw, 25}, lbl)) {
        discard_d.paths = ps;
        dialog = Dialog::ConfirmDiscard;
    }
}

void App::draw_main(const Rect& rc_in) {
    auto& T = ui.theme;
    Rect rc = rc_in;
    r.rect(rc, T.bg);
    if (!rs->error.empty() && !rs->loaded) {
        Rect c{rc.x + rc.w / 2 - 220, rc.y + rc.h / 3, 440, 160};
        r.icon("alert", c.x + c.w / 2 - 12, c.y, 24, T.attention);
        r.draw_text_in(FontId::Bold, 16, {c.x, c.y + 34, c.w, 24}, "Can't find \"" + rs->name + "\"", T.text, Align::Center);
        ui::wrapped_text(ui, {c.x, c.y + 66, c.w, 60}, "It was last seen at " + repo_display_path(rs->path) + ". " + rs->error,
                         FontId::Regular, 12, T.text_muted);
        if (ui::button(ui, ui.id("missing-remove"), {c.x + c.w / 2 - 125, c.y + 120, 120, 25}, "Remove")) remove_repo(current);
        if (ui::button(ui, ui.id("missing-retry"), {c.x + c.w / 2 + 5, c.y + 120, 120, 25}, "Try again", ui::BtnStyle::Primary)) refresh(true);
        return;
    }
    if (rs->status.rewrite) draw_rewrite_banner(rc.cut_top(44));
    else if ((rs->status.merging || rs->status.has_conflicts()) && tab == 0) draw_conflict_banner(rc.cut_top(44));
    if (blame.open && blame.repo == rs->path) { draw_blame_view(rc); return; }
    if (tab == 1) { draw_commit_details(rc); return; }
    if (rs->viewing_stash) { draw_stash_view(rc); return; }
    if (rs->files_sel.size() > 1) { draw_multi_selection(rc); return; }
    if (rs->status.files.empty()) { draw_no_changes(rc); return; }
    const git::StatusEntry* se = nullptr;
    for (auto& f : rs->status.files) if (f.path == rs->selected_file) se = &f;
    if (!se) {
        float cy = std::round(rc.y + rc.h / 2 - 100);
        illustration("empty-no-file-selected", {std::round(rc.x + (rc.w - 288) / 2), cy, 288, 147});
        r.draw_text_in(FontId::Regular, 12, {rc.x, cy + 163, rc.w, 18}, "No file selected", T.text_muted, Align::Center);
        return;
    }
    std::string extra = se->old_path.empty() ? "" : "Renamed from " + se->old_path;
    draw_diff(rc, rs->diff, true, se->path, extra);
}

// Rows for the unified or split (side-by-side) diff layouts.
struct DiffRow {
    const git::DiffLine* left = nullptr;   // unified: the line; split: old side
    const git::DiffLine* right = nullptr;  // split: new side
    int hunk = -1;
    bool header = false;
};

static std::vector<DiffRow> build_rows(const git::FileDiff& d, bool split) {
    std::vector<DiffRow> rows;
    for (size_t h = 0; h < d.hunks.size(); h++) {
        auto& lines = d.hunks[h].lines;
        if (!split) {
            for (auto& l : lines) rows.push_back({&l, nullptr, int(h), l.kind == git::LineKind::HunkHeader});
            continue;
        }
        for (size_t i = 0; i < lines.size();) {
            auto& l = lines[i];
            if (l.kind == git::LineKind::HunkHeader) { rows.push_back({&l, nullptr, int(h), true}); i++; continue; }
            if (l.kind == git::LineKind::Context) { rows.push_back({&l, &l, int(h), false}); i++; continue; }
            // pair a run of deletions with the following run of additions
            size_t j = i;
            std::vector<const git::DiffLine*> dels, adds;
            while (j < lines.size() && lines[j].kind == git::LineKind::Deleted) dels.push_back(&lines[j++]);
            while (j < lines.size() && lines[j].kind == git::LineKind::Added) adds.push_back(&lines[j++]);
            size_t n = std::max(dels.size(), adds.size());
            for (size_t k = 0; k < n; k++) rows.push_back({k < dels.size() ? dels[k] : nullptr, k < adds.size() ? adds[k] : nullptr, int(h), false});
            i = j;
        }
    }
    return rows;
}

static std::string human_size(size_t b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof buf, "%zu B", b);
    else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(b) / 1024);
    else std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / 1024 / 1024);
    return buf;
}

void App::draw_image_diff(const Rect& rc, DiffView& dv) {
    auto& T = ui.theme;
    Rect bar{rc.x, rc.y, rc.w, 48};
    std::vector<std::string> modes = {"2-up", "Swipe", "Onion skin", "Difference"};
    bool both = dv.img_old && dv.img_new;
    if (both) ui::segmented(ui, ui.id("img-mode"), {bar.x + bar.w / 2 - 200, bar.y + 10, 400, 28}, modes, settings.image_mode);
    Rect area{rc.x + 20, bar.bottom() + 10, rc.w - 40, rc.h - bar.h - 70};
    auto fit = [](const gfx::Image& im, const Rect& box) {
        float s = std::min({1.0f, box.w / float(im.w), box.h / float(im.h)});
        float w = float(im.w) * s, h = float(im.h) * s;
        return Rect{box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h};
    };
    auto checkerboard = [&](const Rect& box) {
        r.rect(box, T.dark ? Color::hex(0x30363d) : Color::hex(0xe6e9ed));
        float cs = 8;
        for (float yy = box.y; yy < box.bottom(); yy += cs)
            for (float xx = box.x + float(int((yy - box.y) / cs) % 2) * cs; xx < box.right(); xx += cs * 2)
                r.rect({xx, yy, std::min(cs, box.right() - xx), std::min(cs, box.bottom() - yy)},
                       T.dark ? Color::hex(0x21262d) : Color::hex(0xffffff));
    };
    auto caption = [&](const Rect& img_r, const std::string& title, const gfx::Image& im, size_t bytes, Color col) {
        std::string t = title + "  " + std::to_string(im.w) + " × " + std::to_string(im.h) + "  " + human_size(bytes);
        r.draw_text_in(FontId::Bold, 12, {img_r.x, img_r.bottom() + 8, img_r.w, 20}, t, col, Align::Center);
    };
    if (!dv.img_error.empty()) {
        r.draw_text_in(FontId::Regular, 14, {rc.x, rc.y + rc.h / 3, rc.w, 24}, "Can't preview this image: " + dv.img_error, T.text_muted, Align::Center);
        return;
    }
    int mode = both ? settings.image_mode : 0;
    if (mode == 0) {
        bool two = both;
        float half = two ? (area.w - 30) / 2 : area.w;
        if (dv.img_old) {
            Rect box{area.x, area.y, half, area.h - 30};
            Rect ir = fit(*dv.img_old, box);
            checkerboard(ir);
            r.image(0, *dv.img_old, ir);
            r.rect(ir.inset(-2), {0, 0, 0, 0}, 2, 2, T.danger);
            caption(ir, "Deleted", *dv.img_old, dv.old_bytes, T.danger);
        }
        if (dv.img_new) {
            Rect box{two ? area.x + half + 30 : area.x, area.y, half, area.h - 30};
            Rect ir = fit(*dv.img_new, box);
            checkerboard(ir);
            r.image(0, *dv.img_new, ir);
            r.rect(ir.inset(-2), {0, 0, 0, 0}, 2, 2, T.success);
            caption(ir, "Added", *dv.img_new, dv.new_bytes, T.success);
        }
        return;
    }
    // overlay modes share one frame sized for the larger image
    const gfx::Image& big = (dv.img_new->w * dv.img_new->h >= dv.img_old->w * dv.img_old->h) ? *dv.img_new : *dv.img_old;
    Rect frame = fit(big, {area.x, area.y, area.w, area.h - 50});
    float sx = frame.w / float(big.w), sy = frame.h / float(big.h);
    Rect old_r{frame.x, frame.y, float(dv.img_old->w) * sx, float(dv.img_old->h) * sy};
    Rect new_r{frame.x, frame.y, float(dv.img_new->w) * sx, float(dv.img_new->h) * sy};
    checkerboard(frame);
    if (mode == 1) {  // swipe
        r.image(0, *dv.img_old, old_r);
        float cut = frame.x + frame.w * image_split;
        r.push_clip({cut, frame.y, frame.right() - cut, frame.h});
        r.image(0, *dv.img_new, new_r);
        r.pop_clip();
        r.rect({cut - 1, frame.y - 6, 2, frame.h + 12}, T.accent_emphasis);
        Rect slider{frame.x, frame.bottom() + 16, frame.w, 16};
        r.rect({slider.x, slider.y + 6, slider.w, 4}, T.border, 2);
        float kx = slider.x + slider.w * image_split;
        r.rect({kx - 8, slider.y, 16, 16}, T.accent_emphasis, 8);
        Id sid = ui.id("img-swipe");
        if (ui.pressed(sid, {frame.x - 10, frame.y, frame.w + 20, frame.h + 40})) {}
        if (ui.is_active(sid)) {
            if (ui.mouse_down[0]) { image_split = std::clamp((ui.mx - frame.x) / frame.w, 0.f, 1.f); ui.request_redraw(); }
            else ui.clear_active();
        }
        if (ui.hovered({frame.x - 10, frame.y, frame.w + 20, frame.h + 40})) ui.set_cursor(plat::Cursor::ResizeH);
    } else if (mode == 2) {  // onion skin
        r.image(0, *dv.img_old, old_r);
        r.image(0, *dv.img_new, new_r, image_opacity);
        Rect slider{frame.x + frame.w / 2 - 150, frame.bottom() + 16, 300, 16};
        r.draw_text_in(FontId::Regular, 12, {slider.x - 70, slider.y - 2, 60, 20}, "Deleted", T.danger, Align::Right);
        r.draw_text_in(FontId::Regular, 12, {slider.right() + 10, slider.y - 2, 60, 20}, "Added", T.success);
        r.rect({slider.x, slider.y + 6, slider.w, 4}, T.border, 2);
        float kx = slider.x + slider.w * image_opacity;
        r.rect({kx - 8, slider.y, 16, 16}, T.accent_emphasis, 8);
        Id sid = ui.id("img-onion");
        if (ui.pressed(sid, slider.inset(-10, -6))) {}
        if (ui.is_active(sid)) {
            if (ui.mouse_down[0]) { image_opacity = std::clamp((ui.mx - slider.x) / slider.w, 0.f, 1.f); ui.request_redraw(); }
            else ui.clear_active();
        }
    } else if (dv.img_diff) {  // difference
        r.image(0, *dv.img_diff, {frame.x, frame.y, float(dv.img_diff->w) * sx, float(dv.img_diff->h) * sy});
        r.draw_text_in(FontId::Regular, 12, {frame.x, frame.bottom() + 14, frame.w, 20},
                       "Black areas are unchanged; bright areas show where pixels differ.", T.text_muted, Align::Center);
    }
}

void App::draw_find_bar(const Rect& rc) {
    auto& T = ui.theme;
    Rect bar{rc.right() - 380, rc.y + 8, 360, 40};
    ui.set_layer(40);
    ui.add_blocker(bar, false);
    r.shadow({bar.x, bar.y + 2, bar.w, bar.h}, 8, 10, T.shadow);
    r.rect(bar, T.bg, 8, 1, T.border);
    ui::TextFieldOpts o;
    o.placeholder = "Find in diff";
    o.icon = "search";
    o.autofocus = true;
    bool enter = false;
    if (ui::text_field(ui, ui.id("find-field"), {bar.x + 6, bar.y + 5, 230, 30}, find_text, o, &enter)) find_current = 0;
    if (enter) find_current++;
    if (ui::icon_button(ui, ui.id("find-prev"), {bar.x + 270, bar.y + 6, 28, 28}, "chevron-up", "Previous")) find_current--;
    if (ui::icon_button(ui, ui.id("find-next"), {bar.x + 300, bar.y + 6, 28, 28}, "chevron-down", "Next")) find_current++;
    if (ui::icon_button(ui, ui.id("find-close"), {bar.x + 330, bar.y + 6, 28, 28}, "x", "Close") || ui.take_key(Key::Escape, 0)) {
        find_open = false;
        find_text.clear();
    }
    ui.set_layer(0);
}

void App::draw_diff(const Rect& rc, DiffView& dv, bool selectable_lines, const std::string& path, const std::string& extra) {
    auto& T = ui.theme;
    // .diff-container .header: box-alt, padding 5px 10px, path label + options + status icon
    Rect head = Rect{rc.x, rc.y, rc.w, 31};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    auto [dir, name] = split_path(path);
    float x = head.x + 10;
    x += r.draw_text_in(FontId::Regular, 12, {x, head.y, head.w / 2, head.h}, dir, T.text_muted);
    x += r.draw_text_in(FontId::Bold, 12, {x, head.y, head.w - (x - head.x) - 300, head.h}, name, T.text);
    if (!extra.empty()) r.draw_text_in(FontId::Regular, 12, {x + 12, head.y, 260, head.h}, extra, T.text_muted);
    // right side: diff options (gear) then the file's status octicon
    float rx = head.right() - 10;
    const git::Change* st = nullptr;
    if (rs) {
        for (auto& f : rs->status.files) if (f.path == path && selectable_lines) st = &f.change;
        if (!st) for (auto& f : rs->commit_files) if (f.path == path) st = &f.change;
    }
    if (st) { r.icon(change_icon(*st), rx - 16, head.y + 7, 16, change_color(T, *st)); rx -= 21; }
    Rect gear{rx - 24, head.y + 3, 24, 25};
    auto opts_pid = ui.id("diff-options");
    if (ui::icon_button(ui, ui.id("diff-gear"), gear, "gear", "Diff options")) ui.open_popup(opts_pid);
    // ui/_diff-options.scss popover: "Diff display" radios and "Whitespace" checkbox
    Rect pm{gear.right() - 260, gear.bottom() + 4, 260, 170};
    if (ui::begin_popup(ui, opts_pid, pm)) {
        float px = pm.x + 20, py = pm.y + 20, pw = pm.w - 40;
        r.draw_text_in(FontId::Bold, 12, {px, py, pw, 18}, "Diff display", T.text);
        py += 23;
        bool changed = false;
        if (ui::radio(ui, ui.id("opt-unified"), {px, py, pw, 22}, !settings.split_diff, "Unified")) { settings.split_diff = false; changed = true; }
        py += 22;
        if (ui::radio(ui, ui.id("opt-split"), {px, py, pw, 22}, settings.split_diff, "Split")) { settings.split_diff = true; changed = true; }
        py += 32;
        r.draw_text_in(FontId::Bold, 12, {px, py, pw, 18}, "Whitespace", T.text);
        py += 23;
        bool ws = settings.hide_whitespace;
        if (ui::checkbox_label(ui, ui.id("opt-ws"), {px, py, pw, 22}, ws, "Hide whitespace changes")) {
            settings.hide_whitespace = ws;
            changed = true;
            if (rs) { if (tab == 0 && !rs->selected_file.empty()) select_file(rs->selected_file); if (tab == 1 && !rs->selected_commit_file.empty()) select_commit_file(rs->selected_commit_file); }
        }
        if (changed) save_state();
        ui::end_popup(ui);
    }
    Rect body{rc.x, head.bottom(), rc.w, rc.h - head.h};
    if (dv.path != path) {
        ui::spinner(ui, body.x + body.w / 2, body.y + 60, 8, T.text_muted);
        return;
    }
    if (!dv.error.empty()) {
        r.draw_text_in(FontId::Regular, 12, {body.x, body.y + 40, body.w, 24}, dv.error, T.text_muted, Align::Center);
        return;
    }
    if (dv.image) { draw_image_diff(body, dv); return; }
    if (dv.diff.binary) {
        r.icon("file", body.x + body.w / 2 - 12, body.y + body.h / 3 - 40, 24, T.text_muted);
        r.draw_text_in(FontId::Regular, 14, {body.x, body.y + body.h / 3, body.w, 24}, "This binary file has changed.", T.text_muted, Align::Center);
        return;
    }
    if (dv.diff.too_large) {
        r.draw_text_in(FontId::Regular, 14, {body.x, body.y + body.h / 3, body.w, 24}, "This diff is too large to display.", T.text_muted, Align::Center);
        return;
    }
    if (dv.diff.hunks.empty()) {
        if (dv.loading) ui::spinner(ui, body.x + body.w / 2, body.y + 60, 8, T.text_muted);
        else r.draw_text_in(FontId::Regular, 14, {body.x, body.y + body.h / 3, body.w, 24},
                            dv.ignore_ws ? "Only whitespace changed (hidden)." : "No content changes.", T.text_muted, Align::Center);
        return;
    }
    bool split = settings.split_diff && body.w > 700;
    auto rows = build_rows(dv.diff, split);
    const float RH = 20;  // 16px line + --diff-line-padding-y on both sides
    int maxn = 0;
    for (auto& h : dv.diff.hunks) maxn = std::max({maxn, h.old_start + h.old_count, h.new_start + h.new_count});
    float num_w = std::max(55.f, ui.text_w(FontId::Mono, 11, std::to_string(maxn)) + 20);  // --width-line-number
    // partial selection is unavailable while whitespace is hidden (line indices differ)
    FileSel* fsel = nullptr;
    if (selectable_lines && rs && !dv.ignore_ws) {
        fsel = &rs->sel[path];
        if (!fsel->lines) fsel->lines = std::vector<bool>(dv.full.size(), fsel->included);
        if (fsel->lines->size() != dv.full.size()) fsel->lines = std::vector<bool>(dv.full.size(), true);
    }
    auto line_on = [&](const git::DiffLine* l) {
        return fsel && l && l->kind != git::LineKind::Context && l->index >= 0 && size_t(l->index) < fsel->lines->size() &&
               (*fsel->lines)[size_t(l->index)];
    };
    // find
    std::vector<int> matches;
    std::string needle = git::to_lower(find_text);
    if (find_open && !needle.empty())
        for (int i = 0; i < int(rows.size()); i++) {
            auto& rw = rows[size_t(i)];
            if (rw.header) continue;
            bool hit = (rw.left && git::to_lower(rw.left->text).find(needle) != std::string::npos) ||
                       (rw.right && rw.right != rw.left && git::to_lower(rw.right->text).find(needle) != std::string::npos);
            if (hit) matches.push_back(i);
        }
    Id sid = ui.id("diff-scroll", int64_t(std::hash<std::string>{}(path)));
    if (!matches.empty()) {
        find_current = ((find_current % int(matches.size())) + int(matches.size())) % int(matches.size());
        static int last_scrolled = -1;
        static std::string last_needle;
        if (last_scrolled != matches[size_t(find_current)] || last_needle != needle) {
            ui::scroll_to_visible(ui, sid, float(matches[size_t(find_current)]) * RH - body.h / 3, RH, body.h / 3);
            last_scrolled = matches[size_t(find_current)];
            last_needle = needle;
        }
    }
    if (ui.hovered(body) && ui.scroll_x != 0) {
        diff_hscroll = std::max(0.f, diff_hscroll - ui.scroll_x);
        ui.scroll_x = 0;
    }
    float off = ui::begin_scroll(ui, sid, body);
    int first = std::max(0, int(off / RH));
    float y = body.y - off + float(first) * RH;
    // one "side": gutter (old/new numbers in unified mode, one number in split) + text
    auto draw_side = [&](const Rect& col, const git::DiffLine* l, bool unified, int row_i, int side) {
        float gutter = unified ? num_w * 2 : num_w;
        Rect g{col.x, col.y, gutter, RH};
        if (!l) {  // empty filler on one side of a split row
            r.rect(col, T.diff_empty);
            return;
        }
        bool add = l->kind == git::LineKind::Added, del = l->kind == git::LineKind::Deleted;
        bool on = line_on(l);
        if (add) r.rect(col, T.diff_add_bg);
        else if (del) r.rect(col, T.diff_del_bg);
        Color gbg = add ? T.diff_add_gutter : del ? T.diff_del_gutter : T.diff_gutter;
        if (fsel && (add || del) && on) gbg = add ? T.diff_add_gutter_sel : T.diff_del_gutter_sel;
        r.rect(g, gbg);
        r.line_v(g.right() - 1, g.y, g.bottom(), fsel && (add || del) && on ? gbg.mix(Color::hex(0x000000), 0.1f) : T.diff_gutter_border);
        if (unified) r.line_v(g.x + num_w - 1, g.y, g.bottom(), fsel && (add || del) && on ? gbg : T.diff_gutter_border);
        Color numc = (fsel && on && (add || del)) ? Color::hex(0xffffff) : T.diff_line_number;
        if (unified) {
            if (l->old_no > 0) r.draw_text_in(FontId::Mono, 11, {g.x, col.y, num_w - 10, RH}, std::to_string(l->old_no), numc, Align::Right);
            if (l->new_no > 0) r.draw_text_in(FontId::Mono, 11, {g.x + num_w, col.y, num_w - 10, RH}, std::to_string(l->new_no), numc, Align::Right);
        } else {
            int no = side == 0 ? l->old_no : l->new_no;
            if (no > 0) r.draw_text_in(FontId::Mono, 11, {g.x, col.y, num_w - 10, RH}, std::to_string(no), numc, Align::Right);
        }
        if (fsel && (add || del)) {
            if (ui.hovered(g)) {
                ui.set_cursor(plat::Cursor::Hand);
                r.rect(g, on ? Color::hex(0x000000, 0.08f) : (add ? T.diff_add_gutter_sel : T.diff_del_gutter_sel).with_alpha(0.3f));
            }
            if (ui.pressed(ui.id("gl", l->index), g)) { drag_select_value = on ? 0 : 1; drag_seen.clear(); }
            if (drag_select_value >= 0 && ui.mouse_down[0] && g.contains(ui.mx, ui.my) && !drag_seen.count(l->index)) {
                (*fsel->lines)[size_t(l->index)] = drag_select_value == 1;
                drag_seen.insert(l->index);
                fsel->included = true;
            }
        }
        float tx = col.x + gutter + 5 + ui.text_w(FontId::Mono, 11, "+");
        const char* marker = add ? "+" : del ? "-" : " ";
        r.draw_text(FontId::Mono, 11, col.x + gutter + 5, col.y + 3, marker, T.text);
        r.push_clip({tx - 2, body.y, col.right() - tx, body.h});
        std::string text = expand_tabs(l->text);
        // text selection highlight
        int lo = std::min(text_sel_a, text_sel_b), hi = std::max(text_sel_a, text_sel_b);
        if (text_sel_a >= 0 && row_i >= lo && row_i <= hi) r.rect({tx - 2, col.y, col.right() - tx + 2, RH}, T.accent_emphasis.with_alpha(0.25f));
        // find highlight
        if (!needle.empty()) {
            std::string low = git::to_lower(text);
            bool cur = !matches.empty() && matches[size_t(find_current)] == row_i;
            for (size_t p = low.find(needle); p != std::string::npos; p = low.find(needle, p + 1)) {
                float a = ui.text_w(FontId::Mono, 11, text.substr(0, p)), b = ui.text_w(FontId::Mono, 11, text.substr(0, p + needle.size()));
                r.rect({tx - diff_hscroll + a, col.y + 1, b - a, RH - 2}, cur ? Color::hex(0xff9632, 0.8f) : Color::hex(0xffd33d, 0.45f), 2);
            }
        }
        r.draw_text(FontId::Mono, 11, tx - diff_hscroll, col.y + 3, text, T.text);
        if (l->no_newline) r.icon("circle", tx - diff_hscroll + ui.text_w(FontId::Mono, 11, text) + 6, col.y + 5, 10, T.danger);
        r.pop_clip();
    };
    for (size_t i = size_t(first); i < rows.size(); i++) {
        if (y > body.bottom()) break;
        auto& rw = rows[i];
        Rect lr{body.x, y, body.w, RH};
        if (rw.header) {
            auto& l = *rw.left;
            float gutter = split ? num_w : num_w * 2;
            r.rect(lr, T.diff_hunk_bg);
            Rect hg{lr.x, lr.y, gutter, RH};
            r.rect(hg, T.diff_hunk_gutter);
            r.line_h(lr.x, lr.right(), lr.y, T.diff_hunk_border);
            r.line_h(lr.x, lr.right(), lr.bottom() - 1, T.diff_hunk_border);
            // expand context
            Rect ex{hg.x + 4, hg.y + 2, 16, 16};
            if (ui.hovered(ex)) { r.rect(ex.inset(-2), T.accent_emphasis.with_alpha(0.3f), 3); ui.set_cursor(plat::Cursor::Hand); ui.tooltip(ui.id("expand-tip"), ex, "Expand context"); }
            r.icon("chevron-up", ex.x, ex.y, 16, T.diff_hunk_text);
            if (ui.clicked(ui.id("expand", rw.hunk), ex)) {
                int& ctx = diff_context[path];
                if (ctx < 3) ctx = 3;
                ctx += 20;
                if (rs && tab == 0) select_file(path);
                else if (rs && tab == 1) select_commit_file(path);
            }
            r.draw_text(FontId::Mono, 11, lr.x + gutter + 5 - diff_hscroll, y + 3, l.text, T.diff_hunk_text);
            if (fsel) {
                Rect hit{ex.right() + 4, lr.y, gutter - 24, RH};
                if (ui.hovered(hit)) { ui.set_cursor(plat::Cursor::Hand); r.rect(hit, T.accent_emphasis.with_alpha(0.25f)); }
                if (ui.clicked(ui.id("hunk", rw.hunk), hit)) {
                    auto& h = dv.diff.hunks[size_t(rw.hunk)];
                    bool any_off = false;
                    for (auto& hl : h.lines) if (hl.index >= 0 && hl.kind != git::LineKind::Context && !(*fsel->lines)[size_t(hl.index)]) any_off = true;
                    for (auto& hl : h.lines) if (hl.index >= 0 && hl.kind != git::LineKind::Context) (*fsel->lines)[size_t(hl.index)] = any_off;
                    fsel->included = true;
                }
            }
        } else if (!split) {
            draw_side(lr, rw.left, true, int(i), 0);
        } else {
            float half = std::floor(body.w / 2);
            draw_side({lr.x, y, half, RH}, rw.left, false, int(i), 0);
            draw_side({lr.x + half, y, body.w - half, RH}, rw.right, false, int(i), 1);
            r.line_v(lr.x + half, y, y + RH, T.border);
        }
        y += RH;
    }
    if (!ui.mouse_down[0]) drag_select_value = -1;
    // text selection by dragging in the text area (rows), copied with Cmd/Ctrl+C
    {
        float gutter = split ? num_w : num_w * 2;
        Rect text_area{body.x + gutter + 4, body.y, body.w - gutter - 14, body.h};
        if (ui.hovered(text_area)) ui.set_cursor(plat::Cursor::IBeam);
        int row_at = int((ui.my - body.y + off) / RH);
        if (ui.pressed(ui.id("diff-text-sel"), text_area) && drag_select_value < 0) {
            text_sel_a = text_sel_b = std::clamp(row_at, 0, int(rows.size()) - 1);
            text_selecting = true;
            ui.focus = 0;
        }
        if (text_selecting) {
            if (ui.mouse_down[0]) { text_sel_b = std::clamp(row_at, 0, int(rows.size()) - 1); ui.request_redraw(); }
            else { text_selecting = false; ui.clear_active(); if (text_sel_a == text_sel_b && ui.clicks == 1) {} }
        }
        if (text_sel_a >= 0 && (ui.key_pressed(Key::C, plat::ModCmd))) {
            ui.take_key(Key::C, plat::ModCmd);
            int lo = std::min(text_sel_a, text_sel_b), hi = std::max(text_sel_a, text_sel_b);
            std::string out;
            for (int k = lo; k <= hi && k < int(rows.size()); k++) {
                auto& rw = rows[size_t(k)];
                if (rw.header) continue;
                const git::DiffLine* l = rw.right && split ? rw.right : rw.left;
                if (!l) l = rw.left;
                if (l) out += l->text + "\n";
            }
            ui.set_clipboard(out);
            toast("Copied " + std::to_string(hi - lo + 1) + " line" + (hi == lo ? "" : "s"));
        }
    }
    ui::end_scroll(ui, sid, body, float(rows.size()) * RH + 8);
    if (find_open) {
        draw_find_bar(body);
        if (!needle.empty())
            r.draw_text_in(FontId::Regular, 11, {body.right() - 140, body.y + 14, 40, 28},
                           matches.empty() ? "0/0" : std::to_string(find_current + 1) + "/" + std::to_string(matches.size()), T.text_muted, Align::Right);
    }
    if (ui.hovered(body) && ui.mouse_pressed[1]) {
        std::vector<MenuEntry> items;
        if (text_sel_a >= 0) items.push_back({"Copy", [this] { ui.keys.push_back({Key::C, plat::ModCmd}); }, true, false, plat::os_name() == "macOS" ? "⌘C" : "Ctrl+C"});
        if (fsel) {
            auto sel_copy = *fsel->lines;
            std::string p = path;
            int nsel = 0;
            for (bool b : sel_copy) nsel += b;
            items.push_back({"Discard selected lines…", [this, p, sel_copy] { discard_lines(p, sel_copy); }, nsel > 0});
            items.push_back({"Discard file changes…", [this, p] { discard_d.paths = {p}; dialog = Dialog::ConfirmDiscard; }});
        }
        items.push_back({"Find…", [this] { find_open = true; }, true, false, plat::os_name() == "macOS" ? "⌘F" : "Ctrl+F"});
        open_context(items);
    }
}

void App::draw_avatar(const Rect& rc, const std::string& name, const std::string& email) {
    auto& T = ui.theme;
    std::string key = git::to_lower(git::trim(email));
    if (!key.empty()) {
        auto& e = avatars_[key];
        if (e.state == 0 && std::find(avatar_queue_.begin(), avatar_queue_.end(), key) == avatar_queue_.end()) avatar_queue_.push_back(key);
        if (e.img && e.img->valid()) {
            r.image(gfx::hash_bytes("avatar:" + key), *e.img, rc);
            return;
        }
    }
    // svg.avatar fallback: the person octicon on box-alt
    (void)name;
    r.rect(rc, T.bg_subtle, rc.w / 2, 1, T.border);
    float s = std::round(rc.w * 0.7f);
    r.icon("person", std::round(rc.x + (rc.w - s) / 2), std::round(rc.y + (rc.h - s) / 2 + rc.h * 0.04f), s, T.text_muted);
}

void App::pump_avatars() {
    if (avatar_worker_ || avatar_queue_.empty()) return;
    std::vector<std::string> batch;
    for (auto& k : avatar_queue_) if (avatars_[k].state == 0) { avatars_[k].state = 1; batch.push_back(k); }
    avatar_queue_.clear();
    if (batch.empty()) return;
    avatar_worker_ = true;
    auto alive = alive_;
    std::thread([this, batch, alive] {
        for (auto& email : batch) {
            if (!alive->load()) return;
            auto img = std::make_shared<gfx::Image>();
            try {
                net::Request rq;
                rq.url = "https://avatars.githubusercontent.com/u/e?email=" + net::url_encode(email) + "&s=64";
                rq.headers["User-Agent"] = "Tributary";
                auto resp = net::request(rq);
                if (resp.status == 200 && gfx::is_image_data(resp.body)) {
                    *img = gfx::decode_image(resp.body);
                    // round mask (.avatar { border-radius: 50% }), antialiased
                    float cx = img->w / 2.f, cy = img->h / 2.f, rad = std::min(cx, cy);
                    for (int y = 0; y < img->h; y++)
                        for (int x = 0; x < img->w; x++) {
                            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                            float a = std::clamp(rad - d, 0.f, 1.f);
                            auto& px = img->rgba[(size_t(y) * size_t(img->w) + size_t(x)) * 4 + 3];
                            px = uint8_t(float(px) * a);
                        }
                }
            } catch (...) {}
            if (!alive->load()) return;
            jobs.post([this, email, img] { auto& e = avatars_[email]; e.state = 2; e.img = img; });
        }
        if (alive->load()) jobs.post([this] { avatar_worker_ = false; });
    }).detach();
}

void App::illustration(const std::string& name, const Rect& rc, float alpha, bool dark_filter) {
    bool dark = ui.theme.dark && dark_filter;
    std::string key = dark ? name + "@dark" : name;
    auto& img = illustrations_[key];
    if (!img) {
        img = std::make_shared<gfx::Image>();
        try {
            auto blob = embedded::get(name + ".png");
            *img = gfx::decode_image(std::string_view(reinterpret_cast<const char*>(blob.data), blob.size));
            if (dark) {
                // body.theme-dark .blankslate-image { filter: invert() grayscale(1) brightness(8) contrast(0.6) }
                for (size_t i = 0; i + 3 < img->rgba.size(); i += 4) {
                    float rr = 1 - img->rgba[i] / 255.f, gg = 1 - img->rgba[i + 1] / 255.f, bb = 1 - img->rgba[i + 2] / 255.f;
                    float l = 0.2126f * rr + 0.7152f * gg + 0.0722f * bb;
                    l = std::min(1.f, l * 8);
                    l = (l - 0.5f) * 0.6f + 0.5f;
                    auto v = uint8_t(std::clamp(l, 0.f, 1.f) * 255.f + 0.5f);
                    img->rgba[i] = img->rgba[i + 1] = img->rgba[i + 2] = v;
                }
            }
        } catch (...) {}
    }
    if (img->valid()) r.image(gfx::hash_bytes(key) | 1, *img, rc, alpha);
}

// changes/_changes-interstitial.scss + suggested-actions: 40px padding, 600px column,
// 32px light title, 73x70 paper-stack, stacked action rows with 20px padding
void App::draw_no_changes(const Rect& rc) {
    auto& T = ui.theme;
    float pad = rc.w < 640 ? 20 : 40;
    float w = std::min(600.f, rc.w - 2 * pad);
    float x = std::round(rc.x + (rc.w - w) / 2);
    float y = rc.y + pad;
    illustration("paper-stack", {x + w - 73, y, 73, 70});
    r.draw_text_in(FontId::Regular, 32, {x, y, w - 93, 44}, "No local changes", T.text);
    float th = ui::wrapped_text(ui, {x, y + 46, w - 93, 60},
                                "There are no uncommitted changes in this repository. Here are some friendly suggestions for what to do next.",
                                FontId::Regular, 12, T.text, 1.5f);
    y = std::max(y + 70.f, y + 46 + th) + 20;
    bool mac = plat::os_name() == "macOS", win = plat::os_name() == "Windows";
    auto keys = [&](const std::string& k) { return mac ? k : k; };
    struct Action { std::string title, desc, menu, shortcut, button; bool primary; std::function<void()> fn; std::string id; };
    std::vector<Action> prim, rest;
    std::string remote = remote_name();
    std::string cmd = mac ? "⌘" : "Ctrl", sh = mac ? "⇧" : "Shift";
    auto chord = [&](const std::string& k) { return mac ? cmd + sh + k : cmd + " + " + sh + " + " + k; };
    if (rs->remotes.empty())
        prim.push_back({"Publish your repository to GitHub", "This repository is currently only available on your local machine. By publishing it on GitHub you can share it, and collaborate with others.",
                        "", "", "Publish repository", true,
                        [this] { if (!account.valid()) dialog = Dialog::SignIn; else { publish_d = {}; publish_d.name = rs->name; dialog = Dialog::Publish; } }, "nc-publish"});
    else if (!rs->branch.empty() && rs->upstream.empty() && rs->head)
        prim.push_back({"Publish your branch", "The current branch (" + rs->branch + ") hasn't been published to the remote yet. By publishing it to GitHub you can share it, open a pull request, and collaborate with others.",
                        "Branch menu or", chord("P").substr(0, 0) + (mac ? "⌘P" : "Ctrl + P"), "Publish branch", true, [this] { publish_branch(); }, "nc-pubbranch"});
    else if (rs->behind > 0)
        prim.push_back({"Pull " + std::to_string(rs->behind) + " commit" + (rs->behind == 1 ? "" : "s") + " from the " + remote + " remote",
                        "The current branch (" + rs->branch + ") has commits on GitHub that do not exist on your machine.", "Repository menu or",
                        mac ? "⌘⇧P" : "Ctrl + Shift + P", "Pull " + remote, true, [this] { pull(); }, "nc-pull"});
    else if (rs->ahead > 0)
        prim.push_back({"Push " + std::to_string(rs->ahead) + " commit" + (rs->ahead == 1 ? "" : "s") + " to the " + remote + " remote",
                        "You have " + std::to_string(rs->ahead) + " local commit" + (rs->ahead == 1 ? "" : "s") + " waiting to be pushed to GitHub.",
                        "Repository menu or", mac ? "⌘P" : "Ctrl + P", "Push " + remote, true, [this] { push(); }, "nc-push"});
    else if (rs->github && !rs->branch.empty() && rs->default_branch != rs->branch && !rs->upstream.empty())
        prim.push_back({"Create a Pull Request from your current branch", "The current branch (" + rs->branch + ") is already published to GitHub. Create a pull request to propose and collaborate on your changes.",
                        "Branch menu or", mac ? "⌘R" : "Ctrl + R", "Create Pull Request", true,
                        [this] { plat::open_url(github_url() + "/compare/" + rs->branch + "?expand=1"); }, "nc-pr"});
    rest.push_back({"Open the repository in your external editor", "Select your editor in " + std::string(mac ? "Settings" : win ? "Options" : "Settings"),
                    "Repository menu or", chord("A"), "Open in external editor", false,
                    [this] { plat::open_in_editor(rs->path, settings.editor_cmd); }, "nc-editor"});
    rest.push_back({mac ? "View the files of your repository in Finder" : win ? "View the files of your repository in Explorer" : "View the files of your repository in your File Manager",
                    "", "Repository menu or", chord("F"), mac ? "Show in Finder" : win ? "Show in Explorer" : "Show in your File Manager", false,
                    [this] { plat::reveal_in_file_manager(rs->path); }, "nc-finder"});
    if (rs->github)
        rest.push_back({"Open the repository page on GitHub in your browser", "", "Repository menu or", chord("G"), "View on GitHub", false,
                        [this] { plat::open_url(github_url()); }, "nc-gh"});
    (void)keys;
    auto group = [&](std::vector<Action>& acts) {
        if (acts.empty()) return;
        std::vector<float> hs;
        float total = 0;
        for (auto& a : acts) {
            float bw = std::max(120.f, ui.text_w(FontId::Regular, 12, a.button) + 20);
            float tw = w - 40 - bw - 20;
            float h2 = a.desc.empty() ? 0 : ui::wrapped_text(ui, {0, 0, tw, 200}, a.desc, FontId::Regular, 12, T.text, 1.5f, true) + 5;
            float h = std::max(25.f, 18 + h2 + (a.menu.empty() ? 0 : 18)) + 40;
            hs.push_back(h);
            total += h;
        }
        bool primary = acts[0].primary;
        Color bg = primary ? (T.dark ? Color::hex(0x05264c) : Color::hex(0xf1f8ff)) : T.bg;
        Color bc = primary ? (T.dark ? Color::hex(0x044289) : Color::hex(0xc8e1ff)) : T.border;
        r.rect({x, y, w, total}, bg, 6, 1, bc);
        for (size_t i = 0; i < acts.size(); i++) {
            auto& a = acts[i];
            float h = hs[i];
            Rect row{x, y, w, h};
            if (i) r.line_h(row.x, row.right(), row.y, bc);
            float bw = std::max(120.f, ui.text_w(FontId::Regular, 12, a.button) + 20);
            float tw = w - 40 - bw - 20;
            float ty = row.y + 20;
            r.draw_text_in(FontId::Bold, 12, {row.x + 20, ty, tw, 18}, a.title, T.text);
            ty += 18;
            if (!a.desc.empty()) ty += ui::wrapped_text(ui, {row.x + 20, ty, tw, 200}, a.desc, FontId::Regular, 12, T.text, 1.5f) + 5;
            if (!a.menu.empty()) {
                float mw = r.draw_text_in(FontId::Regular, 12, {row.x + 20, ty, tw, 18}, a.menu + " ", T.text_muted);
                r.draw_text_in(FontId::Regular, 12, {row.x + 20 + mw, ty, tw - mw, 18}, a.shortcut, T.text);
            }
            if (ui::button(ui, ui.id(a.id), {row.right() - 20 - bw, std::round(row.y + (h - 25) / 2), bw, 25}, a.button,
                           a.primary ? ui::BtnStyle::Primary : ui::BtnStyle::Default))
                a.fn();
            y += h;
        }
    };
    if (!prim.empty()) { group(prim); y += 10; }
    group(rest);
}

void App::draw_commit_details(const Rect& rc) {
    auto& T = ui.theme;
    if (!rs->selected_commit) {
        illustration("empty-no-commit", {std::round(rc.x + (rc.w - 288) / 2), std::round(rc.y + rc.h / 2 - 100), 288, 138});
        r.draw_text_in(FontId::Regular, 12, {rc.x, std::round(rc.y + rc.h / 2 + 48), rc.w, 18}, "No commit selected", T.text_muted, Align::Center);
        return;
    }
    const git::CommitInfo* ci = nullptr;
    for (auto& c : rs->history) if (c.id == *rs->selected_commit) ci = &c;
    // history/_commit-summary.scss: 14px semibold title (padding 10), monospace 11px
    // description clamped to 3 lines, then an 11px meta row with 16px avatar/octicons
    std::string body = ci ? ci->commit.body() : "";
    float title_h = 36;
    float body_h = 0;
    float lh11 = r.text().line_height(FontId::Mono, 11) * 1.25f;
    if (!body.empty()) body_h = std::min(lh11 * 3, ui::wrapped_text(ui, {0, 0, rc.w - 20, 0}, body, FontId::Mono, 11, T.text, 1.5f, true)) + 20;
    float meta_h = 26;
    Rect head{rc.x, rc.y, rc.w, title_h + meta_h + body_h};
    r.rect(head, T.bg);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    if (ci) {
        std::string summ = ci->commit.summary();
        r.draw_text_in(FontId::Bold, 14, {head.x + 10, head.y + 10, head.w - 20, 16}, summ.empty() ? "Empty commit message" : summ,
                       summ.empty() ? T.text_muted : T.text);
        float y = head.y + title_h;
        // meta row
        float x = head.x + 10;
        draw_avatar({x, y, 16, 16}, ci->commit.author.name, ci->commit.author.email);
        x += 19;
        x += r.draw_text_in(FontId::Regular, 11, {x, y, 300, 16}, ci->commit.author.name, T.text) + 10;
        r.icon("git-commit", x, y, 16, T.text_muted);
        x += 19;
        Rect sha{x, y, ui.text_w(FontId::Regular, 11, ci->id.short_hex()) + 2, 16};
        if (ui.hovered(sha)) { ui.set_cursor(plat::Cursor::Hand); ui.tooltip(ui.id("sha-tip"), sha, "Click to copy " + ci->id.hex()); }
        if (ui.clicked(ui.id("sha"), sha)) { ui.set_clipboard(ci->id.hex()); toast("Copied commit SHA"); }
        r.draw_text_in(FontId::Regular, 11, sha, ci->id.short_hex(), T.text);
        x = sha.right() + 10;
        r.icon("file-diff", x, y, 16, T.text_muted);
        x += 19;
        x += r.draw_text_in(FontId::Regular, 11, {x, y, 200, 16},
                            std::to_string(rs->commit_files.size()) + " changed file" + (rs->commit_files.size() == 1 ? "" : "s"), T.text) + 10;
        r.icon("clock", x, y, 16, T.text_muted);
        x += 19;
        x += r.draw_text_in(FontId::Regular, 11, {x, y, 220, 16}, git::format_relative_time(ci->commit.author.time, git::unix_now()), T.text) + 10;
        bool signed_commit = false;
        for (auto& [k, v] : ci->commit.extra_headers) if (k == "gpgsig" || k == "gpgsig-sha256") signed_commit = true;
        if (signed_commit) {
            Rect sb{x, y, ui.text_w(FontId::Regular, 11, "Signed") + 24, 16};
            r.icon("lock", sb.x, sb.y, 16, T.success);
            r.draw_text_in(FontId::Regular, 11, {sb.x + 19, sb.y, sb.w, 16}, "Signed", T.success);
            if (ui.hovered(sb)) ui.tooltip(ui.id("signed-tip"), sb, "This commit carries a cryptographic signature (GPG, SSH or X.509).");
        }
        y += meta_h;
        if (!body.empty()) {
            r.line_h(head.x, head.right(), y, T.border);
            r.push_clip({head.x, y + 10, head.w, body_h - 20});
            ui::wrapped_text(ui, {head.x + 10, y + 10, head.w - 20, body_h}, body, FontId::Mono, 11, T.text, 1.5f);
            r.pop_clip();
        }
    }
    Rect content{rc.x, head.bottom(), rc.w, rc.h - head.h};
    float lw = std::min(250.f, content.w * 0.32f);
    Rect files{content.x, content.y, lw, content.h};
    r.line_v(files.right(), files.y, files.bottom(), T.border);
    float off = ui::begin_scroll(ui, ui.id("commit-files"), files);
    float y = files.y - off;
    int i = 0;
    for (auto& f : rs->commit_files) {
        Rect row{files.x, y, files.w, 29};
        if (row.bottom() >= files.y && row.y <= files.bottom()) {
            bool rc_click = false;
            if (ui::selectable(ui, ui.id("cf", i), row, f.path == rs->selected_commit_file, nullptr, &rc_click)) select_commit_file(f.path);
            r.line_h(row.x, row.right(), row.bottom() - 1, T.border);
            auto [dir, name] = split_path(f.path);
            Rect tr{row.x + 10, row.y, row.w - 46, row.h - 1};
            float nw = ui.text_w(FontId::Regular, 12, name);
            if (nw + 20 >= tr.w || dir.empty()) r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, f.path, tr.w), T.text);
            else {
                float dw = r.draw_text_in(FontId::Regular, 12, tr, r.ellipsize_middle(FontId::Regular, 12, dir, tr.w - nw), T.text_muted);
                r.draw_text_in(FontId::Regular, 12, {tr.x + dw, tr.y, tr.w - dw, tr.h}, name, T.text);
            }
            r.icon(change_icon(f.change), row.right() - 26, row.y + 6, 16, change_color(T, f.change));
            if (rc_click) {
                std::string p = f.path;
                bool deleted = f.change == git::Change::Deleted;
                open_context({{"Blame at this commit", [this, p] { open_blame(p, *rs->selected_commit); }, !deleted},
                              {"", {}, true, true},
                              {"Copy file path", [this, p] { ui.set_clipboard(rs->path + "/" + p); }},
                              {"Copy relative file path", [this, p] { ui.set_clipboard(p); }},
                              {"View on GitHub", [this, p] { plat::open_url(github_url() + "/blob/" + rs->selected_commit->hex() + "/" + p); }, rs->github.has_value()}});
            }
        }
        y += 29;
        i++;
    }
    if (rs->commit_files.empty() && ci)
        r.draw_text_in(FontId::Regular, 12, {files.x, files.y + 20, files.w, 24}, "No changed files", T.text_muted, Align::Center);
    ui::end_scroll(ui, ui.id("commit-files"), files, y + off - files.y);
    Rect dr{files.right() + 1, content.y, content.w - lw - 1, content.h};
    if (!rs->selected_commit_file.empty()) {
        std::string extra;
        for (auto& f : rs->commit_files) if (f.path == rs->selected_commit_file && !f.old_path.empty()) extra = "Renamed from " + f.old_path;
        draw_diff(dr, rs->commit_diff, false, rs->selected_commit_file, extra);
    }
}

void App::draw_stash_view(const Rect& rc) {
    auto& T = ui.theme;
    Rect head{rc.x, rc.y, rc.w, 56};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    r.draw_text_in(FontId::Bold, 15, {head.x + 20, head.y, head.w - 320, head.h}, "Stashed changes", T.text);
    if (ui::button(ui, ui.id("stash-restore"), {head.right() - 290, head.y + 12, 130, 32}, "Restore", ui::BtnStyle::Primary, !has_changes() && !jobs.busy()))
        stash_restore(true);
    if (has_changes() && ui.hovered({head.right() - 290, head.y + 12, 130, 32})) ui.tooltip(ui.id("stash-restore-tip"), {head.right() - 290, head.y + 12, 130, 32}, "Commit or discard your current changes before restoring");
    if (ui::button(ui, ui.id("stash-discard"), {head.right() - 150, head.y + 12, 130, 32}, "Discard", ui::BtnStyle::Danger, !jobs.busy()))
        stash_drop();
    Rect content{rc.x, head.bottom(), rc.w, rc.h - head.h};
    float lw = std::min(250.f, content.w * 0.32f);
    Rect files{content.x, content.y, lw, content.h};
    r.line_v(files.right(), files.y, files.bottom(), T.border);
    float y = files.y;
    int i = 0;
    for (auto& f : rs->stash_files) {
        Rect row{files.x, y, files.w, 30};
        if (ui::selectable(ui, ui.id("sf", i++), row, f.path == rs->selected_stash_file)) {
            rs->selected_stash_file = f.path;
            try {
                auto repo = git::Repository::open(git::git_to_path(rs->path));
                rs->stash_diff.diff = repo.commit_file_diff(f);
                rs->stash_diff.path = f.path;
            } catch (...) {}
        }
        r.draw_text_in(FontId::Regular, 12, {row.x + 14, row.y, row.w - 50, row.h}, r.ellipsize_middle(FontId::Regular, 12, f.path, row.w - 50), T.text);
        r.icon(change_icon(f.change), row.right() - 28, row.y + 7, 16, change_color(T, f.change));
        y += 30;
    }
    if (!rs->selected_stash_file.empty())
        draw_diff({files.right() + 1, content.y, content.w - lw - 1, content.h}, rs->stash_diff, false, rs->selected_stash_file, "");
}

// ui/_no-repositories.scss + no-repositories-view.tsx
void App::draw_welcome(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg);
    bool mac = plat::os_name() == "macOS";
    // graphics: top (right 80, top 40, max 20%) and bottom (right 10, bottom 10, max 40%)
    {
        float th = std::min(rc.h * 0.2f, 88.f), tw = th * 42.3f / 43.8f;
        illustration("welcome-illustration-left-top", {rc.right() - 80 - tw, rc.y + 40, tw, th}, 1.f, false);
        float bh = std::min(rc.h * 0.4f, 344.f), bw = bh * 114.f / 172.3f;
        if (bw > rc.w * 0.4f) { bw = rc.w * 0.4f; bh = bw * 172.3f / 114.f; }
        illustration("welcome-illustration-left-bottom", {rc.right() - 10 - bw, rc.bottom() - 10 - bh, bw, bh}, 1.f, false);
    }
    float pad = rc.h < 400 ? 10 : 60;
    float x = rc.x + pad, w = rc.w - 2 * pad, y = rc.y + pad;
    r.draw_text_in(FontId::Regular, 42, {x, y, w, 56}, "Let's get started!", T.text);
    y += 58;
    r.draw_text_in(FontId::Regular, 12, {x, y, w, 18}, "Add a repository to Tributary to start collaborating", T.text);
    y += 18 + 40;
    bool signed_in = account.valid();
    float pane = std::floor(w / 2);
    Rect left{x, y, pane - 10, rc.bottom() - pad - y};
    Rect acts = signed_in ? Rect{x + pane + 10, y, pane - 10, left.h} : left;
    if (signed_in) {
        // .content-pane.repository-list: filter, grouped list, "Clone <repo>" button
        ui::TextFieldOpts fo;
        fo.placeholder = "Filter your repositories";
        fo.icon = "search";
        static std::string wfilter;
        ui::text_field(ui, ui.id("w-filter"), {left.x, left.y, left.w, 25}, wfilter, fo);
        Rect list{left.x, left.y + 35, left.w, left.h - 35 - 35};
        r.rect(list, T.bg, 0, 1, T.border);
        if (gh_repos_loading && gh_repos.empty()) ui::spinner(ui, list.x + list.w / 2, list.y + 40, 8, T.text_muted);
        float off = ui::begin_scroll(ui, ui.id("w-repos"), list.inset(1));
        float yy = list.y + 1 - off;
        std::string f = git::to_lower(wfilter);
        r.draw_text_in(FontId::Bold, 12, {list.x + 10, yy, list.w - 20, 29}, "Your repositories", T.text);
        yy += 29;
        int i = 0;
        static int wsel = -1;
        for (auto& gr : gh_repos) {
            if (!f.empty() && git::to_lower(gr.full_name).find(f) == std::string::npos) { i++; continue; }
            Rect row{list.x + 1, yy, list.w - 2, 29};
            if (row.bottom() >= list.y && row.y <= list.bottom()) {
                bool sel = wsel == i;
                if (sel) r.rect(row, T.row_selected_focus);
                else if (ui.hovered(row)) r.rect(row, T.row_hover);
                bool dbl = false;
                if (ui::row_click(ui, ui.id("wr", i), row, &dbl)) {
                    wsel = i;
                    if (dbl) {
                        clone_d = {};
                        clone_d.tab = 0;
                        clone_d.url = gr.clone_url;
                        clone_d.path = settings.clone_dir + "/" + gr.name;
                        clone_d.selected = i;
                        dialog = Dialog::Clone;
                    }
                }
                Color fg = sel ? T.text_inverse : T.text;
                r.icon(gr.is_private ? "lock" : gr.fork ? "repo-forked" : "repo", row.x + 10, row.y + 6, 16, fg);
                r.draw_text_in(FontId::Regular, 12, {row.x + 36, row.y, row.w - 46, row.h}, gr.full_name, fg);
            }
            yy += 29;
            i++;
        }
        ui::end_scroll(ui, ui.id("w-repos"), list.inset(1), yy + off - list.y);
        bool has_sel = wsel >= 0 && wsel < int(gh_repos.size());
        std::string lbl = has_sel ? "Clone " + gh_repos[size_t(wsel)].full_name : "Clone repository";
        if (ui::button(ui, ui.id("w-clone-sel"), {left.x, left.bottom() - 25, left.w, 25}, lbl, ui::BtnStyle::Primary, has_sel)) {
            auto& gr = gh_repos[size_t(wsel)];
            clone_d = {};
            clone_d.tab = 0;
            clone_d.url = gr.clone_url;
            clone_d.path = settings.clone_dir + "/" + gr.name;
            clone_d.selected = wsel;
            dialog = Dialog::Clone;
        }
    }
    // .button-group: full-width buttons, 24px octicon, left-aligned label
    float by = acts.y;
    auto big = [&](const char* icon, const std::string& label, std::function<void()> fn, Id id) {
        Rect b{acts.x, by, acts.w, 54};
        bool hov = ui.hovered(b);
        r.rect(b, hov ? T.btn_hover : T.btn_bg, 6, 1, T.btn_border);
        r.icon(icon, b.x + 15, b.y + 15, 24, T.text);
        r.draw_text_in(FontId::Regular, 12, {b.x + 54, b.y, b.w - 64, b.h}, label, T.text);
        if (hov) ui.set_cursor(plat::Cursor::Hand);
        if (ui.clicked(id, b)) fn();
        by += 64;
    };
    big("repo-clone", mac ? "Clone a Repository from the Internet…" : "Clone a repository from the Internet…",
        [this] { clone_d = {}; clone_d.tab = account.valid() ? 0 : 1; dialog = Dialog::Clone; load_gh_repos(); }, ui.id("w-clone"));
    big("plus", mac ? "Create a New Repository on your Local Drive…" : "Create a New Repository on your local drive…",
        [this] { create_d = {}; create_d.path = settings.clone_dir; dialog = Dialog::Create; }, ui.id("w-create"));
    big("file-directory", mac ? "Add an Existing Repository from your Local Drive…" : "Add an Existing Repository from your local drive…",
        [this] { add_d = {}; dialog = Dialog::AddExisting; }, ui.id("w-add"));
    big("code", mac ? "Edit a Repository on GitHub without Cloning…" : "Edit a repository on GitHub without cloning…",
        [this] {
            if (!account.valid()) { ghe_d.signin_tab = 0; dialog = Dialog::SignIn; return; }
            ide_open_d = {};
            dialog = Dialog::IdeOpen;
            load_gh_repos();
        }, ui.id("w-ide"));
    // .drag-drop-info
    std::string tip = "You can drag & drop an existing repository folder here to add it to Tributary";
    float th = ui::wrapped_text(ui, {0, 0, acts.w - 66, 100}, tip, FontId::Regular, 12, T.text, 1.5f, true);
    Rect tipr{acts.x, by, acts.w, th + 40};
    r.rect(tipr, T.accent_emphasis.with_alpha(0.06f), 6, 2, T.dark ? Color::hex(0x044289) : Color::hex(0xc8e1ff));
    r.icon("light-bulb", tipr.x + 20, tipr.y + 21, 16, T.text);
    float pw = r.draw_text_in(FontId::Bold, 12, {tipr.x + 46, tipr.y + 20, 80, 18}, "ProTip!", T.text);
    ui::wrapped_text(ui, {tipr.x + 46, tipr.y + 20, tipr.w - 66, th}, std::string(size_t(std::ceil(pw / ui.text_w(FontId::Regular, 12, " "))) + 1, ' ') + tip,
                     FontId::Regular, 12, T.text, 1.5f);
    if (!signed_in) {
        Rect si{acts.x, tipr.bottom() + 20, acts.w, 25};
        float sw = r.draw_text_in(FontId::Regular, 12, si, "Have a GitHub account? ", T.text_muted);
        if (ui::button(ui, ui.id("w-signin"), {si.x + sw, si.y, ui.text_w(FontId::Regular, 12, "Sign in") + 12, 25}, "Sign in", ui::BtnStyle::Link))
            dialog = Dialog::SignIn;
    }
}

// ================================================================ shortcuts & toasts
void App::handle_shortcuts() {
    if (ui.input_layer() >= 60 || dialog != Dialog::None) return;
    if (ui.take_key(Key::Grave, plat::ModCtrl)) { toggle_terminal(); return; }
    if (terminal_focus_id_ && ui.focus == terminal_focus_id_) return;
    // every menu accelerator works as a shortcut (Edit's clipboard items are handled by text fields)
    for (auto& mm : build_menus()) {
        if (mm.title == "Edit" || mm.title == "Tributary" || mm.title == "Window") continue;
        for (auto& it : mm.items) {
            if (it.sep || it.native || it.key == Key::Unknown || !it.fn) continue;
            if (ui.focus != 0 && it.key == Key::Backspace) continue;  // word-delete in text fields
            if (ui.key_pressed(it.key, it.mods)) {
                ui.take_key(it.key, it.mods);
                if (it.enabled) { auto fn = it.fn; fn(); }
                if (dialog != Dialog::None) return;
            }
        }
    }
    auto cmd = plat::ModCmd;
    auto shift = plat::ModShift;
    if (ui.take_key(Key::N, cmd)) { create_d = {}; create_d.path = settings.clone_dir; dialog = Dialog::Create; }
    if (ui.take_key(Key::O, cmd)) { add_d = {}; dialog = Dialog::AddExisting; }
    if (ui.take_key(Key::O, uint8_t(cmd | shift))) { clone_d = {}; clone_d.tab = account.valid() ? 0 : 1; dialog = Dialog::Clone; load_gh_repos(); }
    if (ui.take_key(Key::Comma, cmd)) { settings_d = {}; dialog = Dialog::Settings; }
    if (ui.take_key(Key::T, cmd)) { ui.open_popup(ui.id("repo-popup")); repo_filter.clear(); load_repo_indicators(); }
    if (ui.take_key(Key::Equals, cmd) || ui.take_key(Key::Equals, uint8_t(cmd | shift))) set_zoom(settings.zoom + 0.1f);
    if (ui.take_key(Key::Minus, cmd)) set_zoom(settings.zoom - 0.1f);
    if (ui.take_key(Key::Num0, cmd)) set_zoom(1.0f);
    if (!rs) return;
    if (ui.take_key(Key::Num1, cmd)) tab = 0;
    if (ui.take_key(Key::Num2, cmd)) { tab = 1; if (!rs->selected_commit && !rs->history.empty()) select_commit(rs->history[0].id); }
    if (ui.take_key(Key::B, cmd)) { ui.open_popup(ui.id("branch-popup")); branch_filter.clear(); load_prs(); }
    if (ui.take_key(Key::N, uint8_t(cmd | shift))) { branch_d = {}; dialog = Dialog::NewBranch; }
    if (ui.take_key(Key::M, uint8_t(cmd | shift))) { merge_d = {}; merge_d.mode = 0; dialog = Dialog::Merge; refresh(true); }
    if (ui.take_key(Key::H, uint8_t(cmd | shift))) { merge_d = {}; merge_d.mode = 1; dialog = Dialog::Merge; refresh(true); }
    if (ui.take_key(Key::E, uint8_t(cmd | shift))) { merge_d = {}; merge_d.mode = 2; dialog = Dialog::Merge; refresh(true); }
    if (ui.take_key(Key::F, cmd)) { find_open = true; ui.focus = 0; }
    if (ui.take_key(Key::P, uint8_t(cmd | plat::ModAlt))) {
        pr_d = {};
        pr_d.base = rs->default_branch.empty() ? "main" : rs->default_branch;
        dialog = Dialog::PrPreview;
        load_pr_preview();
    }
    if (ui.take_key(Key::P, cmd)) push();
    if (ui.take_key(Key::P, uint8_t(cmd | shift))) pull();
    if (ui.take_key(Key::T, uint8_t(cmd | shift))) fetch();
    if (ui.take_key(Key::F, uint8_t(cmd | shift))) plat::reveal_in_file_manager(rs->path);
    if (ui.take_key(Key::A, uint8_t(cmd | shift))) plat::open_in_editor(rs->path, settings.editor_cmd);
    if (ui.take_key(Key::G, uint8_t(cmd | shift)) && rs->github) plat::open_url(github_url());
    if (ui.take_key(Key::R, cmd) && rs->github && !rs->branch.empty()) plat::open_url(github_url() + "/compare/" + rs->branch + "?expand=1");
    if (ui.take_key(Key::F5, 0)) refresh(true);
    if (ui.take_key(Key::Backspace, uint8_t(cmd | shift)) && has_changes()) {
        discard_d.paths.clear();
        for (auto& f : rs->status.files) discard_d.paths.push_back(f.path);
        dialog = Dialog::ConfirmDiscard;
    }
}

void App::draw_toasts() {
    auto& T = ui.theme;
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](auto& t) { return t.until < ui.time; }), toasts.end());
    float y = r.height() - 20;
    ui.set_layer(95);
    for (int i = int(toasts.size()) - 1; i >= 0; i--) {
        auto& t = toasts[size_t(i)];
        float aw = t.action ? ui.text_w(FontId::Bold, 12, t.action_label) + 28 : 0;
        float w = std::min(560.f, ui.text_w(FontId::Regular, 12, t.text) + 56 + aw);
        Rect b{r.width() - w - 20, y - 42, w, 42};
        ui.add_blocker(b, false);
        r.shadow({b.x, b.y + 4, b.w, b.h}, 8, 12, T.shadow);
        r.rect(b, T.tooltip_bg, 8);
        r.icon(t.error ? "alert" : "check", b.x + 14, b.y + 13, 16, t.error ? Color::hex(0xff8182) : Color::hex(0x4ac26b));
        r.draw_text_in(FontId::Regular, 12, {b.x + 40, b.y, b.w - 52 - aw, b.h}, t.text, T.tooltip_text);
        if (t.action) {
            Rect ab{b.right() - aw - 8, b.y + 7, aw, 28};
            bool hov = ui.hovered(ab);
            r.rect(ab, hov ? Color::hex(0xffffff, 0.2f) : Color::hex(0xffffff, 0.1f), 6);
            r.draw_text_in(FontId::Bold, 12, ab, t.action_label, Color::hex(0x58a6ff), Align::Center);
            if (ui.clicked(ui.id("toast-act", i), ab)) { auto fn = t.action; t.until = 0; fn(); continue; }
        }
        if (ui.clicked(ui.id("toast", i), b)) t.until = 0;
        y -= 50;
    }
    if (!toasts.empty()) ui.animate();
    ui.set_layer(0);
}

}  // namespace app
