// SPDX-License-Identifier: MIT
// Features beyond GitHub Desktop: the command palette, history search, blame, and
// AI-written commit messages.
#include <algorithm>
#include <cmath>
#include <thread>

#include "app.hpp"

namespace app {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using plat::Key;
namespace fs = std::filesystem;

// ================================================================ command palette
namespace {

struct PaletteItem {
    std::string group, label, hint;
    std::function<void()> fn;
};

// Fuzzy subsequence match: higher is better, <0 means no match. Rewards consecutive
// letters and matches at word starts.
int fuzzy(const std::string& text, const std::string& q) {
    if (q.empty()) return 0;
    std::string t = git::to_lower(text);
    int score = 0, run = 0;
    size_t ti = 0;
    for (size_t qi = 0; qi < q.size(); qi++) {
        char c = q[qi];
        if (c == ' ') { run = 0; continue; }
        size_t found = t.find(c, ti);
        if (found == std::string::npos) return -1;
        bool word_start = found == 0 || !std::isalnum(static_cast<unsigned char>(t[found - 1]));
        run = found == ti ? run + 1 : 1;
        score += 1 + run * 2 + (word_start ? 6 : 0) - int(std::min<size_t>(found - ti, 10));
        ti = found + 1;
    }
    return score + std::max(0, 30 - int(t.size()) / 2);
}

}  // namespace

void App::open_palette() {
    palette = Palette{};
    palette.open = true;
    ui.close_all_popups();
    ui.focus = ui.id("palette-q");
}

void App::draw_palette() {
    auto& T = ui.theme;
    auto& P = palette;
    // gather everything you can do from here
    std::vector<PaletteItem> items;
    for (auto& m : build_menus()) {
        std::string group = m.title;
        group.erase(std::remove(group.begin(), group.end(), '&'), group.end());
        if (group == "Edit" || group == "Window") continue;  // clipboard items act on the focused field
        for (auto& it : m.items) {
            if (it.sep || !it.enabled || !it.fn || it.native < 0) continue;
            std::string label = it.label;
            label.erase(std::remove(label.begin(), label.end(), '&'), label.end());
            items.push_back({group, label, accel_text(it.key, it.mods), it.fn});
        }
    }
    if (rs) {
        for (auto& b : rs->branches) {
            if (b.current) continue;
            std::string name = b.name;
            items.push_back({"Branch", (b.remote ? "Check out " : "Switch to ") + name, b.remote ? "remote" : "", [this, name] { checkout(name); }});
        }
    }
    for (size_t i = 0; i < repos.size(); i++) {
        if (int(i) == current) continue;
        int idx = int(i);
        items.push_back({"Repository", "Open " + (repos[i].owner.empty() ? "" : repos[i].owner + "/") + repos[i].name, "", [this, idx] { open_repo(idx); }});
    }
    std::string q = git::to_lower(git::trim(P.query));
    std::vector<std::pair<int, size_t>> ranked;
    for (size_t i = 0; i < items.size(); i++) {
        int sc = fuzzy(items[i].label, q);
        if (q.size() && sc < 0) sc = fuzzy(items[i].group + " " + items[i].label, q) - 4;
        if (sc >= 0 || q.empty()) ranked.push_back({q.empty() ? 0 : sc, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (ranked.size() > 200) ranked.resize(200);
    P.sel = std::clamp(P.sel, 0, std::max(0, int(ranked.size()) - 1));

    // overlay near the top, like a command bar
    ui.set_layer(80);
    float W = r.width(), H = r.height();
    ui.add_blocker({0, 0, W, H}, true);
    r.rect({0, 0, W, H}, Color{0, 0, 0, T.dark ? 0.35f : 0.15f});
    float pw = std::min(600.f, W - 40), rows = std::min<float>(10, float(ranked.size()));
    Rect box{std::round((W - pw) / 2), 70, pw, 54 + std::max(1.f, rows) * 34 + 8};
    r.shadow({box.x, box.y + 4, box.w, box.h}, 8, 18, T.shadow);
    r.rect(box, T.bg, 8, 1, T.border);
    auto qid = ui.id("palette-q");
    ui.focus = qid;
    if (ui.key_pressed(Key::Down)) { ui.take_key(Key::Down); P.sel = std::min(int(ranked.size()) - 1, P.sel + 1); }
    if (ui.key_pressed(Key::Up)) { ui.take_key(Key::Up); P.sel = std::max(0, P.sel - 1); }
    bool run = false;
    if (ui.key_pressed(Key::Enter, 0)) { ui.take_key(Key::Enter, 0); run = true; }
    bool close = false;
    if (ui.key_pressed(Key::Escape)) { ui.take_key(Key::Escape); close = true; }
    ui::TextFieldOpts o;
    o.placeholder = "Type a command, a branch or a repository";
    o.icon = "search";
    std::string before = P.query;
    ui::text_field(ui, qid, {box.x + 12, box.y + 12, box.w - 24, 30}, P.query, o);
    if (P.query != before) P.sel = 0;
    float y = box.y + 54;
    int first = std::max(0, P.sel - 9);
    for (int k = first; k < int(ranked.size()) && k < first + 10; k++) {
        auto& it = items[ranked[size_t(k)].second];
        Rect row{box.x + 6, y, box.w - 12, 32};
        bool hov = ui.hovered(row);
        if (hov && (ui.mx != P.mx || ui.my != P.my)) P.sel = k;
        bool sel = k == P.sel;
        if (sel) r.rect(row, T.row_selected_focus, 5);
        Color fg = sel ? T.row_selected_text : T.text, dim = sel ? T.row_selected_text : T.text_muted;
        float gx = row.x + 12;
        float gw = r.draw_text_in(FontId::Regular, 12, {gx, row.y, 120, row.h}, it.group, dim);
        r.draw_text_in(FontId::Regular, 12, {gx + gw + 8, row.y, row.w - gw - 120, row.h},
                       r.ellipsize(FontId::Regular, 12, it.label, row.w - gw - 140), fg);
        if (!it.hint.empty()) r.draw_text_in(FontId::Regular, 11, {row.right() - 110, row.y, 100, row.h}, it.hint, dim, Align::Right);
        if (ui.clicked(ui.id("palette-row", int64_t(k)), row)) { P.sel = k; run = true; }
        y += 34;
    }
    P.mx = ui.mx;
    P.my = ui.my;
    if (ranked.empty()) r.draw_text_in(FontId::Regular, 12, {box.x, y, box.w, 32}, "Nothing matches \"" + P.query + "\"", T.text_muted, Align::Center);
    if (ui.mouse_pressed[0] && !box.contains(ui.mx, ui.my)) close = true;
    ui.set_layer(0);
    if (run && !ranked.empty()) {
        auto fn = items[ranked[size_t(P.sel)].second].fn;
        P.open = false;
        ui.focus = 0;
        fn();
    } else if (close) {
        P.open = false;
        ui.focus = 0;
    }
}

// ================================================================ history search
void App::run_history_search() {
    if (!rs) return;
    std::string q = git::trim(history_query);
    history_query_ran = q;
    if (q.empty()) { history_hits.clear(); history_searching = false; return; }
    history_searching = true;
    auto path = rs->path;
    jobs.run("Searching history", [this, path, q] {
        std::vector<git::CommitInfo> hits;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            if (auto h = repo.head()) hits = repo.search_log(*h, q, 1000);
        } catch (...) {}
        auto shared = std::make_shared<std::vector<git::CommitInfo>>(std::move(hits));
        jobs.post([this, path, q, shared] {
            if (!rs || rs->path != path || history_query_ran != q) return;
            history_hits = std::move(*shared);
            history_searching = false;
        });
    });
}

void App::draw_history_search(const Rect& rc) {
    auto& T = ui.theme;
    const float RH = 50;
    auto lid = ui.id("history-hits");
    if (history_searching && history_hits.empty()) {
        ui::spinner(ui, rc.x + rc.w / 2, rc.y + 40, 7, T.text_muted);
        return;
    }
    if (history_hits.empty()) {
        r.draw_text_in(FontId::Regular, 12, {rc.x, rc.y + 24, rc.w, 20}, "No commits match \"" + history_query_ran + "\"", T.text_muted, Align::Center);
        return;
    }
    float off = ui::begin_scroll(ui, lid, rc);
    float y = rc.y - off;
    int64_t now = git::unix_now();
    for (size_t i = 0; i < history_hits.size(); i++, y += RH) {
        if (y + RH < rc.y || y > rc.bottom()) continue;
        auto& c = history_hits[i];
        Rect row{rc.x, y, rc.w, RH};
        bool sel = rs->selected_commit && *rs->selected_commit == c.id;
        if (ui::row_click(ui, ui.id("hit", int64_t(i)), row)) {
            // make sure the commit details can find it, then show it
            if (std::none_of(rs->history.begin(), rs->history.end(), [&](auto& h) { return h.id == c.id; })) rs->history.push_back(c);
            rs->history_sel.clear();
            select_commit(c.id);
        }
        if (sel) r.rect(row, T.row_selected_focus);
        else if (ui.hovered(row)) r.rect(row, T.row_hover);
        r.line_h(row.x, row.right(), row.bottom() - 1, T.border);
        Color fg = sel ? T.row_selected_text : T.text, dim = sel ? T.row_selected_text : T.text_muted;
        r.draw_text_in(FontId::Bold, 12, {row.x + 10, row.y + 8, row.w - 20, 16}, r.ellipsize(FontId::Bold, 12, c.commit.summary(), row.w - 20), fg);
        std::string meta = c.commit.author.name + " · " + git::format_relative_time(c.commit.author.time, now) + " · " + c.id.short_hex();
        r.draw_text_in(FontId::Regular, 11, {row.x + 10, row.y + 27, row.w - 20, 16}, r.ellipsize(FontId::Regular, 11, meta, row.w - 20), dim);
    }
    ui::end_scroll(ui, lid, rc, float(history_hits.size()) * RH);
}

// ================================================================ blame
void App::open_blame(const std::string& path, const ObjectId& at) {
    if (!rs) return;
    blame = BlameState{};
    blame.open = true;
    blame.loading = true;
    blame.repo = rs->path;
    blame.path = path;
    blame.at = at;
    auto repo_path = rs->path;
    jobs.run("Blame", [this, repo_path, path, at] {
        auto st = std::make_shared<BlameState>();
        try {
            auto repo = git::Repository::open(git::git_to_path(repo_path));
            st->lines = repo.blame(path, at, &st->commits);
        } catch (const std::exception& e) {
            st->error = e.what();
        }
        jobs.post([this, st, repo_path, path] {
            if (!blame.open || blame.repo != repo_path || blame.path != path) return;
            blame.lines = std::move(st->lines);
            blame.commits = std::move(st->commits);
            blame.error = st->error;
            blame.loading = false;
        });
    });
}

void App::draw_blame_view(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg);
    // header like the diff header: path, the commit it's at, close
    Rect head{rc.x, rc.y, rc.w, 33};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    r.icon("history", head.x + 10, head.y + 9, 16, T.text_muted);
    float tw = r.draw_text_in(FontId::Bold, 12, {head.x + 34, head.y, head.w - 200, head.h}, blame.path, T.text);
    r.draw_text_in(FontId::Regular, 12, {head.x + 42 + tw, head.y, 200, head.h}, "blame at " + blame.at.short_hex(), T.text_muted);
    if (ui::icon_button(ui, ui.id("blame-close"), {head.right() - 34, head.y + 5, 24, 23}, "x", "Close blame") || ui.key_pressed(Key::Escape)) {
        if (ui.key_pressed(Key::Escape)) ui.take_key(Key::Escape);
        blame.open = false;
        return;
    }
    Rect body{rc.x, head.bottom(), rc.w, rc.h - head.h};
    if (blame.loading) { ui::spinner(ui, body.x + body.w / 2, body.y + 60, 8, T.text_muted); return; }
    if (!blame.error.empty()) {
        r.draw_text_in(FontId::Regular, 12, {body.x, body.y + 40, body.w, 20}, blame.error, T.danger, Align::Center);
        return;
    }
    const float LH = 20, gutter = std::min(320.f, body.w * 0.38f);
    auto lid = ui.id("blame-list");
    float off = ui::begin_scroll(ui, lid, body);
    float y = body.y - off;
    int64_t now = git::unix_now();
    // alternate a faint band per run of lines from the same commit
    bool band = false;
    float numw = ui.text_w(FontId::Mono, 11, std::to_string(blame.lines.size())) + 16;
    for (size_t i = 0; i < blame.lines.size(); i++, y += LH) {
        auto& l = blame.lines[i];
        bool start = i == 0 || blame.lines[i - 1].commit != l.commit;
        if (start) band = !band;
        if (y + LH < body.y || y > body.bottom()) continue;
        Rect row{body.x, y, body.w, LH};
        Rect g{body.x, y, gutter, LH};
        if (band) r.rect(g, T.bg_subtle);
        if (start && i) r.line_h(g.x, row.right(), y, T.border_muted);
        auto ci = blame.commits.find(l.commit);
        if (start && ci != blame.commits.end()) {
            auto& c = ci->second;
            std::string meta = git::format_relative_time(c.author.time, now);
            float mw = ui.text_w(FontId::Regular, 11, meta);
            r.draw_text_in(FontId::Mono, 11, {g.x + 10, y, 64, LH}, l.commit.short_hex(), T.accent);
            r.draw_text_in(FontId::Regular, 11, {g.x + 70, y, g.w - 84 - mw, LH},
                           r.ellipsize(FontId::Regular, 11, c.author.name + "  " + c.summary(), g.w - 90 - mw), T.text);
            r.draw_text_in(FontId::Regular, 11, {g.right() - 10 - mw, y, mw, LH}, meta, T.text_muted);
            if (ui.hovered(g)) {
                ui.set_cursor(plat::Cursor::Hand);
                ui.tooltip(ui.id("blame-tip", int64_t(i)), g, c.summary() + " · " + c.author.name + " · " + l.commit.short_hex() + " (click to open it in History)");
            }
            if (ui.clicked(ui.id("blame-commit", int64_t(i)), g)) {
                // open it in History
                tab = 1;
                if (std::none_of(rs->history.begin(), rs->history.end(), [&](auto& h) { return h.id == l.commit; })) rs->history.push_back({l.commit, c});
                rs->history_sel.clear();
                select_commit(l.commit);
                blame.open = false;
                ui::end_scroll(ui, lid, body, float(blame.lines.size()) * LH);
                return;
            }
        }
        r.line_v(g.right(), y, y + LH, T.border);
        r.draw_text_in(FontId::Mono, 11, {g.right(), y, numw - 6, LH}, std::to_string(i + 1), T.diff_line_number, Align::Right);
        std::string text = l.text;
        if (!text.empty() && text.back() == '\r') text.pop_back();
        for (size_t k = 0; (k = text.find('\t', k)) != std::string::npos;) text.replace(k, 1, "    ");
        r.draw_text_in(FontId::Mono, 12, {g.right() + numw + 6, y, row.w - gutter - numw - 10, LH}, text, T.text);
    }
    ui::end_scroll(ui, lid, body, float(blame.lines.size()) * LH + 8);
}

// ================================================================ AI commit messages
void App::generate_commit_message() {
    if (!rs || ai_msg_running) return;
    auto cfg = ai_config();
    if (ai::needs_key(cfg.provider) && cfg.key.empty()) {
        toast("Add an AI provider's API key in Settings → AI to write commit messages.", true);
        settings_d = {};
        settings_d.tab = 5;
        dialog = Dialog::Settings;
        return;
    }
    if (cfg.model.empty()) { toast("Pick a model for the assistant first (Settings → AI).", true); return; }
    std::vector<git::StatusEntry> files;
    for (auto& f : rs->status.files) {
        auto it = rs->sel.find(f.path);
        if (it == rs->sel.end() || it->second.included) files.push_back(f);
    }
    if (files.empty()) { toast("Select the files to commit first.", true); return; }
    ai_msg_running = true;
    ai_msg_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = ai_msg_cancel;
    auto path = rs->path;
    std::string branch = rs->branch;
    auto alive = alive_;
    std::thread([this, cfg, files, path, branch, cancel, alive] {
        std::string diff;
        try {
            auto repo = git::Repository::open(git::git_to_path(path));
            for (auto& f : files) {
                if (diff.size() > 60000) { diff += "\n(… more files not shown)\n"; break; }
                auto d = repo.working_diff(f, 2);
                if (d.binary) { diff += "Binary file " + f.path + " changed\n"; continue; }
                std::string p = git::unified_patch(f.path, d);
                if (p.size() > 12000) p = p.substr(0, 12000) + "\n(… diff truncated)\n";
                diff += p;
            }
        } catch (const std::exception& e) {
            diff = std::string("(couldn't read the diff: ") + e.what() + ")";
        }
        std::string sys =
            "You write git commit messages. Reply with only the message: a summary line in the imperative mood, at most 60 "
            "characters, no trailing period; then, if the change needs explaining, a blank line and a short description "
            "wrapped at 72 characters saying what changed and why. No markdown, no code fences, no quotes.";
        std::vector<ai::Message> h{{ai::Message::User, "Branch: " + branch + "\n\nWrite the commit message for this diff:\n\n" + diff, {}, {}, {}}};
        std::string text;
        auto turn = ai::complete(
            cfg, sys, h, {},
            [this, alive, &text](const std::string& s) {
                text += s;
                std::string t = text;
                if (!alive->load()) return;
                jobs.post([this, t] {
                    if (!rs) return;
                    std::string clean = git::trim(t);
                    auto nl = clean.find('\n');
                    rs->summary = git::trim(clean.substr(0, nl));
                    rs->description = nl == std::string::npos ? "" : git::trim(clean.substr(nl + 1));
                });
            },
            [cancel, alive] { return !cancel->load() && alive->load(); });
        if (!alive->load()) return;
        jobs.post([this, turn] {
            ai_msg_running = false;
            if (!turn.error.empty()) toast("Couldn't write a commit message: " + turn.error, true);
        });
    }).detach();
}

}  // namespace app
