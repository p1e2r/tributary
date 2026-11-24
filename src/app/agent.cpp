// SPDX-License-Identifier: MIT
// The AI assistant in the GitHub editor: a tool-using agent (Claude, GPT, Gemini or
// Ollama) that lists, reads, searches and edits the repository's files. Its edits land
// in the same change set as the user's and go out in the same pull request.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "app.hpp"
#include "../net/http.hpp"

namespace app {

using gfx::Align;
using gfx::Color;
using gfx::FontId;
using plat::Key;
using ui::Id;

// ================================================================ settings
std::string App::ai_key(int p) { return plat::secret_get(std::string("ai:") + ai::provider_short(ai::Provider(p))); }
void App::set_ai_key(int p, const std::string& key) {
    std::string k = std::string("ai:") + ai::provider_short(ai::Provider(p));
    if (key.empty()) plat::secret_delete(k);
    else plat::secret_set(k, key);
}

ai::Config App::ai_config() {
    ai::Config c;
    int p = settings.ai_provider;
    c.provider = ai::Provider(p);
    c.key = ai_key(p);
    c.model = settings.ai_model[p];
    c.base_url = settings.ai_base[p];
    return c;
}

void App::agent_load_models(int p, bool force) {
    if (agent.models_loading.count(p) || (!force && agent.models.count(p))) return;
    ai::Config c;
    c.provider = ai::Provider(p);
    c.key = ai_key(p);
    c.base_url = settings.ai_base[p];
    if (ai::needs_key(c.provider) && c.key.empty()) return;
    agent.models_loading.insert(p);
    auto alive = alive_;
    std::thread([this, c, p, alive] {
        std::vector<std::string> models;
        std::string err;
        try { models = ai::list_models(c); } catch (const std::exception& e) { err = e.what(); }
        if (!alive->load()) return;
        jobs.post([this, p, models, err] {
            agent.models_loading.erase(p);
            agent.models[p] = models;
            agent.models_error[p] = err;
            if (settings.ai_model[p].empty() && !models.empty()) {
                // pick a sensible default: the first model whose name looks like a flagship chat model
                std::string pick = models.front();
                const char* prefs[] = {"claude-sonnet", "claude-opus", "gpt-5", "gpt-4.1", "gpt-4o", "gemini-2.5-pro", "gemini-3", "gemini", "llama", "qwen"};
                for (auto* pr : prefs) {
                    auto it = std::find_if(models.rbegin(), models.rend(), [&](auto& m) { return m.find(pr) != std::string::npos; });
                    if (it != models.rend()) { pick = *it; break; }
                }
                settings.ai_model[p] = pick;
                save_state();
            }
        });
    }).detach();
}

// ================================================================ tools
static std::vector<ai::ToolDef> agent_tools() {
    auto obj = [](json::Object props, json::Array req) {
        return json::Value(json::Object{{"type", "object"}, {"properties", props}, {"required", req}});
    };
    auto str = [](const char* d) { return json::Value(json::Object{{"type", "string"}, {"description", d}}); };
    auto num = [](const char* d) { return json::Value(json::Object{{"type", "integer"}, {"description", d}}); };
    return {
        {"list_files", "List file paths in the repository, optionally only under a directory.",
         obj({{"directory", str("Only list files under this directory (optional)")}}, {})},
        {"read_file", "Read a file. Returns numbered lines. Use offset/limit for big files.",
         obj({{"path", str("File path relative to the repository root")}, {"offset", num("First line to return, 1-based (optional)")},
              {"limit", num("How many lines (optional, default 400)")}},
             {"path"})},
        {"search_files", "Search for text in the repository. Case-insensitive substring match.",
         obj({{"query", str("Text to search for")}, {"directory", str("Only search under this directory (optional)")}}, {"query"})},
        {"edit_file", "Replace an exact snippet in a file. old_string must match exactly once (include surrounding lines to make it unique) unless replace_all is true.",
         obj({{"path", str("File path")}, {"old_string", str("Exact text to replace")}, {"new_string", str("Replacement text")},
              {"replace_all", json::Object{{"type", "boolean"}, {"description", "Replace every occurrence"}}}},
             {"path", "old_string", "new_string"})},
        {"write_file", "Create a file or overwrite it completely.",
         obj({{"path", str("File path")}, {"content", str("The complete new contents")}}, {"path", "content"})},
        {"delete_file", "Delete a file.", obj({{"path", str("File path")}}, {"path"})},
    };
}

// Run fn on the UI thread and wait for its result (the agent runs on a worker).
template <class F>
static auto on_ui(App& a, F fn, const std::shared_ptr<std::atomic<bool>>& alive) -> decltype(fn()) {
    using R = decltype(fn());
    auto p = std::make_shared<std::promise<R>>();
    auto f = p->get_future();
    a.jobs.post([p, fn]() mutable {
        try { p->set_value(fn()); } catch (...) { p->set_exception(std::current_exception()); }
    });
    while (f.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (!alive->load()) throw std::runtime_error("cancelled");
    return f.get();
}

static std::string short_diff(const std::string& a, const std::string& b, int max_lines = 14) {
    auto d = git::diff_text(a, b, 1);
    std::string out;
    int n = 0, add = 0, del = 0;
    for (auto& h : d.hunks)
        for (auto& l : h.lines) {
            if (l.kind == git::LineKind::Added) add++;
            if (l.kind == git::LineKind::Deleted) del++;
            if (l.kind == git::LineKind::HunkHeader) continue;
            if (n < max_lines) {
                out += (l.kind == git::LineKind::Added ? "+" : l.kind == git::LineKind::Deleted ? "-" : " ") + l.text + "\n";
                n++;
            }
        }
    if (n >= max_lines) out += "…\n";
    return "+" + std::to_string(add) + " −" + std::to_string(del) + "\n" + out;  // first line: the stat
}

std::string App::agent_run_tool(const ai::ToolCall& call, const std::shared_ptr<std::atomic<bool>>& cancel) {
    auto alive = alive_;
    auto keep = std::make_shared<std::atomic<bool>>(true);
    // stop waiting if the app closes or the user presses Stop
    std::thread([alive, cancel, keep] {
        while (keep->load()) {
            if (!alive->load() || cancel->load()) { keep->store(false); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
    struct Done { std::shared_ptr<std::atomic<bool>> k; ~Done() { k->store(false); } } done{keep};
    auto& in = call.input;
    std::string path = git::trim(in["path"].str());
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    auto note = [&](const std::string& text, const std::string& target, bool ok = true) {
        on_ui(*this, [this, text, target, ok] { agent.items.push_back({AgentItem::Tool, text, target, ok}); return 0; }, keep);
    };
    // make sure a file's text is loaded; returns its text (throws when missing/binary)
    auto ensure_text = [&](const std::string& p) -> std::string {
        struct Info { bool exists = false, loaded = false, binary = false; std::string sha, text; };
        auto info = on_ui(*this, [this, p] {
            Info i;
            auto it = ide.files.find(p);
            if (it == ide.files.end() || it->second.deleted) return i;
            auto& f = it->second;
            i.exists = true;
            i.loaded = f.state == IdeFile::Loaded;
            i.binary = f.binary;
            i.sha = f.added ? f.copy_from_sha : f.base_sha;
            if (i.loaded && !f.binary) i.text = f.doc.text();
            return i;
        }, keep);
        if (!info.exists) throw std::runtime_error("There's no file at " + p + ".");
        if (info.binary) throw std::runtime_error(p + " is a binary file.");
        if (info.loaded) return info.text;
        std::string data = info.sha.empty() ? "" : gh::get_blob(account, ide.owner, ide.repo, info.sha);
        if (data.find('\0') != std::string::npos) throw std::runtime_error(p + " is a binary file.");
        return on_ui(*this, [this, p, data] {
            auto& f = ide.files[p];
            if (f.state != IdeFile::Loaded) {
                f.state = IdeFile::Loaded;
                f.doc.set_text(data, p);
                f.original = f.added ? "" : data;
            }
            return f.doc.text();
        }, keep);
    };
    // ask the user before changing files (unless they allowed everything for this chat)
    auto approve = [&](const std::string& title, const std::string& p, const std::string& preview) -> bool {
        bool ask = on_ui(*this, [this] { return settings.ai_ask && !agent.allow_all; }, keep);
        if (!ask) return true;
        auto ans = std::make_shared<std::promise<int>>();
        auto fut = ans->get_future();
        on_ui(*this, [this, title, p, preview, ans] { agent.approval = AgentApproval{title, p, preview, ans}; win.wake(); return 0; }, keep);
        while (fut.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
            if (!keep->load()) { on_ui(*this, [this] { agent.approval.reset(); return 0; }, alive); throw std::runtime_error("cancelled"); }
        int a = fut.get();
        if (a == 2) on_ui(*this, [this] { agent.allow_all = true; return 0; }, keep);
        return a > 0;
    };
    auto set_text = [&](const std::string& p, const std::string& text) {
        on_ui(*this, [this, p, text] {
            if (!ide.files.count(p) || ide.files[p].deleted) ide_new_file(p);
            auto& f = ide.files[p];
            auto& d = f.doc;
            d.undo.push_back({d.lines, d.cursor, d.anchor});
            auto undo = std::move(d.undo);
            float sy = d.scroll_y;
            auto cur = d.cursor;
            d.set_text(text, p);
            d.undo = std::move(undo);
            d.scroll_y = sy;
            d.cursor = d.anchor = cur;
            f.checked_version = ~0ull;
            ide.tree_ver++;
            ide.change_diff_ver = ~0ull;
            return 0;
        }, keep);
    };

    const std::string& name = call.name;
    if (name == "list_files") {
        std::string dir = git::trim(in["directory"].str());
        while (!dir.empty() && (dir.back() == '/' || dir[0] == '/')) dir = dir.back() == '/' ? dir.substr(0, dir.size() - 1) : dir.substr(1);
        auto files = on_ui(*this, [this, dir] {
            std::vector<std::string> out;
            for (auto& [p, f] : ide.files)
                if (!f.deleted && (dir.empty() || p.rfind(dir + "/", 0) == 0)) out.push_back(p);
            return out;
        }, keep);
        std::string out;
        for (size_t i = 0; i < files.size() && i < 3000; i++) out += files[i] + "\n";
        if (files.size() > 3000) out += "… and " + std::to_string(files.size() - 3000) + " more\n";
        note("Listed " + std::to_string(files.size()) + " files" + (dir.empty() ? "" : " in " + dir), "");
        return out.empty() ? "(no files)" : out;
    }
    if (name == "read_file") {
        std::string text = ensure_text(path);
        int offset = std::max(1, int(in["offset"].integer(1)));
        int limit = int(in["limit"].integer(400));
        if (limit <= 0) limit = 400;
        std::string out;
        int ln = 1, shown = 0, total = 0;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos && pos == text.size()) break;
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            total++;
            if (ln >= offset && shown < limit) {
                out += std::to_string(ln) + "\t" + line + "\n";
                shown++;
            }
            ln++;
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (offset + shown - 1 < total) out += "… (" + std::to_string(total) + " lines total; use offset to read more)\n";
        note("Read " + path, path);
        return out.empty() ? "(empty file)" : out;
    }
    if (name == "search_files") {
        std::string q = git::to_lower(in["query"].str()), dir = git::trim(in["directory"].str());
        if (q.empty()) throw std::runtime_error("query is empty");
        // search what's loaded locally (including edits), then GitHub's code index for the rest
        auto local = on_ui(*this, [this, q, dir] {
            std::vector<std::string> hits;
            std::set<std::string> loaded;
            for (auto& [p, f] : ide.files) {
                if (f.deleted || f.state != IdeFile::Loaded || f.binary || (!dir.empty() && p.rfind(dir, 0) != 0)) continue;
                loaded.insert(p);
                for (size_t i = 0; i < f.doc.lines.size() && hits.size() < 200; i++)
                    if (git::to_lower(f.doc.lines[i]).find(q) != std::string::npos)
                        hits.push_back(p + ":" + std::to_string(i + 1) + ": " + git::trim(f.doc.lines[i]).substr(0, 200));
            }
            return std::make_pair(hits, loaded);
        }, keep);
        std::string out;
        for (auto& h : local.first) out += h + "\n";
        try {
            net::Request rq;
            rq.url = account.endpoint + "/search/code?per_page=50&q=" + net::url_encode(in["query"].str() + " repo:" + ide.owner + "/" + ide.repo);
            rq.headers["Authorization"] = "Bearer " + account.token;
            rq.headers["Accept"] = "application/vnd.github+json";
            auto r = net::request(rq);
            if (r.status == 200) {
                auto v = json::parse(r.body);
                std::string more;
                for (auto& item : v["items"].arr()) {
                    std::string p = item["path"].str();
                    if (local.second.count(p) || (!dir.empty() && p.rfind(dir, 0) != 0)) continue;
                    more += p + "\n";
                }
                if (!more.empty()) out += "\nAlso matched in GitHub's index of " + ide.default_branch + " (read the file to see where):\n" + more;
            }
        } catch (...) {}
        note("Searched for \"" + in["query"].str() + "\"", "");
        return out.empty() ? "No matches." : out;
    }
    if (name == "edit_file") {
        std::string text = ensure_text(path);
        std::string olds = in["old_string"].str(), news = in["new_string"].str();
        if (olds.empty()) throw std::runtime_error("old_string is empty; use write_file to create files");
        size_t count = 0;
        for (size_t p = text.find(olds); p != std::string::npos; p = text.find(olds, p + olds.size())) count++;
        if (count == 0) throw std::runtime_error("old_string wasn't found in " + path + ". Read the file again and copy the text exactly.");
        bool all = in["replace_all"].boolean(false);
        if (count > 1 && !all) throw std::runtime_error("old_string matches " + std::to_string(count) + " places in " + path + "; include more surrounding lines.");
        std::string out = text;
        for (size_t p = out.find(olds); p != std::string::npos; p = out.find(olds, p + news.size())) {
            out.replace(p, olds.size(), news);
            if (!all) break;
        }
        std::string prev = short_diff(text, out);
        if (!approve("Edit " + path, path, prev)) { note("You declined an edit to " + path, path, false); return "The user declined this edit."; }
        set_text(path, out);
        note("Edited " + path + "  " + prev.substr(0, prev.find('\n')), path);
        return "Edited " + path + ".";
    }
    if (name == "write_file") {
        bool exists = on_ui(*this, [this, path] { return ide.files.count(path) && !ide.files[path].deleted; }, keep);
        std::string old = exists ? ensure_text(path) : "";
        std::string content = in["content"].str();
        if (!exists) {
            size_t sl = 0;
            bool bad = path.empty() || path.find("..") != std::string::npos || path.find('\\') != std::string::npos;
            (void)sl;
            if (bad) throw std::runtime_error("invalid path");
        }
        std::string prev = short_diff(old, content);
        if (!approve((exists ? "Overwrite " : "Create ") + path, path, prev)) { note("You declined writing " + path, path, false); return "The user declined this change."; }
        set_text(path, content);
        note((exists ? "Rewrote " : "Created ") + path + "  " + prev.substr(0, prev.find('\n')), path);
        return (exists ? "Overwrote " : "Created ") + path + ".";
    }
    if (name == "delete_file") {
        bool exists = on_ui(*this, [this, path] { return ide.files.count(path) && !ide.files[path].deleted; }, keep);
        if (!exists) throw std::runtime_error("There's no file at " + path + ".");
        if (!approve("Delete " + path, path, "")) { note("You declined deleting " + path, path, false); return "The user declined this deletion."; }
        on_ui(*this, [this, path] { ide_delete(path); return 0; }, keep);
        note("Deleted " + path, path);
        return "Deleted " + path + ".";
    }
    throw std::runtime_error("Unknown tool " + name);
}

// ================================================================ the loop
void App::agent_send(const std::string& text) {
    if (agent.running || git::trim(text).empty()) return;
    auto cfg = ai_config();
    if (ai::needs_key(cfg.provider) && cfg.key.empty()) { agent.items.push_back({AgentItem::Error, "Add an API key first (Settings → AI).", "", false}); return; }
    if (cfg.model.empty()) { agent.items.push_back({AgentItem::Error, "Pick a model first.", "", false}); return; }
    agent.items.push_back({AgentItem::User, text, "", true});
    agent.history.push_back({ai::Message::User, text, {}, {}, {}});
    agent.running = true;
    agent.cancel = std::make_shared<std::atomic<bool>>(false);
    agent.scroll_to_bottom = 1;
    // context for the model: the repo, and what the user is looking at
    std::string sys = "You are a coding agent inside Tributary, a Git client, working in its GitHub editor on the repository " + ide.owner + "/" +
                      ide.repo + " (base branch " + ide.base + "). The files live on GitHub; use the tools to list, read, search and change "
                      "them. Every change you make is collected with the user's own edits, shown in the Changes tab, and submitted as one "
                      "pull request when the user chooses Create pull request. You cannot commit, push or run commands. Read a file "
                      "before editing it, prefer edit_file for targeted changes, and keep explanations short.";
    if (!ide.active.empty() && ide.files.count(ide.active)) {
        auto& f = ide.files[ide.active];
        sys += "\n\nThe user has " + ide.active + " open";
        if (f.state == IdeFile::Loaded && !f.binary) {
            sys += ", cursor on line " + std::to_string(f.doc.cursor.line + 1);
            if (f.doc.has_selection()) {
                std::string sel = f.doc.selected_text();
                if (sel.size() > 4000) sel = sel.substr(0, 4000) + "…";
                sys += ", with this selected:\n```\n" + sel + "\n```";
            }
        }
        sys += ".";
    }
    auto changes = ide_changes();
    if (!changes.empty()) {
        sys += "\nFiles changed so far in this session:";
        for (auto& [p, k] : changes) sys += " " + p;
    }
    auto history = agent.history;
    auto cancel = agent.cancel;
    auto alive = alive_;
    std::thread([this, cfg, sys, history, cancel, alive]() mutable {
        auto tools = agent_tools();
        std::string fatal;
        for (int step = 0; step < 40; step++) {
            if (cancel->load() || !alive->load()) break;
            // a fresh assistant bubble that fills in as text streams
            jobs.post([this] { agent.items.push_back({AgentItem::Assistant, "", "", true}); });
            auto turn = ai::complete(
                cfg, sys, history, tools,
                [this, alive](const std::string& s) {
                    if (!alive->load()) return;
                    jobs.post([this, s] {
                        for (auto it = agent.items.rbegin(); it != agent.items.rend(); ++it)
                            if (it->kind == AgentItem::Assistant) { it->text += s; break; }
                        agent.scroll_to_bottom = 1;
                    });
                },
                [cancel, alive] { return !cancel->load() && alive->load(); });
            if (!alive->load()) return;
            if (!turn.error.empty() && !cancel->load()) { fatal = turn.error; break; }
            ai::Message m;
            m.role = ai::Message::Assistant;
            m.text = turn.text;
            m.calls = turn.calls;
            m.raw = turn.raw;
            history.push_back(m);
            if (turn.calls.empty() || cancel->load()) break;
            ai::Message results;
            results.role = ai::Message::Tools;
            for (auto& tc : turn.calls) {
                ai::ToolResult r{tc.id, tc.name, "", false};
                try {
                    r.content = agent_run_tool(tc, cancel);
                } catch (const std::exception& e) {
                    r.content = std::string("Error: ") + e.what();
                    r.error = true;
                    if (std::string(e.what()) != "cancelled")
                        jobs.post([this, tc, msg = std::string(e.what())] { agent.items.push_back({AgentItem::Tool, tc.name + " failed: " + msg, "", false}); });
                }
                results.results.push_back(r);
                if (cancel->load()) break;
            }
            history.push_back(results);
        }
        if (!alive->load()) return;
        jobs.post([this, history, fatal, cancel] {
            agent.history = history;
            agent.running = false;
            agent.approval.reset();
            // drop empty assistant bubbles (turns that only called tools)
            agent.items.erase(std::remove_if(agent.items.begin(), agent.items.end(),
                                             [](auto& i) { return i.kind == AgentItem::Assistant && git::trim(i.text).empty(); }),
                              agent.items.end());
            if (!fatal.empty()) agent.items.push_back({AgentItem::Error, fatal, "", false});
            if (cancel->load()) agent.items.push_back({AgentItem::Error, "Stopped.", "", false});
            agent.scroll_to_bottom = 1;
        });
    }).detach();
}

// ================================================================ panel
// Minimal markdown: paragraphs, ``` code blocks, and `inline code` kept as is.
static float draw_markdown(App& a, const Rect& rc, const std::string& text, bool measure) {
    auto& T = a.ui.theme;
    float y = rc.y;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t fence = text.find("```", pos);
        std::string para = text.substr(pos, fence == std::string::npos ? std::string::npos : fence - pos);
        // paragraphs
        std::string p = git::trim(para);
        if (!p.empty()) {
            std::string clean;
            for (size_t i = 0; i < p.size(); i++) {
                if (p.compare(i, 2, "**") == 0) { i++; continue; }
                if (p[i] == '`') continue;
                clean += p[i];
            }
            y += ui::wrapped_text(a.ui, {rc.x, y, rc.w, 4000}, clean, FontId::Regular, 12, T.text, 1.5f, measure) + 6;
        }
        if (fence == std::string::npos) break;
        size_t nl = text.find('\n', fence);
        size_t end = nl == std::string::npos ? std::string::npos : text.find("```", nl);
        std::string code = nl == std::string::npos ? "" : text.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1);
        while (!code.empty() && code.back() == '\n') code.pop_back();
        float ch = ui::wrapped_text(a.ui, {0, 0, rc.w - 16, 4000}, code, FontId::Mono, 11, T.text, 1.4f, true) + 14;
        if (!measure) {
            a.r.rect({rc.x, y, rc.w, ch}, T.bg_subtle, 6, 1, T.border);
            ui::wrapped_text(a.ui, {rc.x + 8, y + 7, rc.w - 16, ch}, code, FontId::Mono, 11, T.text, 1.4f);
        }
        y += ch + 8;
        if (end == std::string::npos) break;
        pos = end + 3;
    }
    return y - rc.y;
}

void App::draw_agent_panel(const Rect& rc) {
    auto& T = ui.theme;
    r.rect(rc, T.bg);
    r.line_v(rc.x, rc.y, rc.bottom(), T.border);
    int p = settings.ai_provider;
    // header
    Rect head{rc.x + 1, rc.y, rc.w - 1, 33};
    r.rect(head, T.bg_subtle);
    r.line_h(head.x, head.right(), head.bottom() - 1, T.border);
    r.icon("sparkle-fill", head.x + 10, head.y + 8, 16, T.accent_emphasis);
    r.draw_text_in(FontId::Bold, 12, {head.x + 32, head.y, 100, head.h}, "Assistant", T.text);
    auto mpid = ui.id("ai-model-popup");
    std::string model = settings.ai_model[p].empty() ? "Choose model" : settings.ai_model[p];
    float mw = std::min(head.w - 190, ui.text_w(FontId::Regular, 11, model) + 34);
    Rect mb{head.right() - 66 - mw, head.y + 5, mw, 23};
    if (ui::button(ui, ui.id("ai-model-btn"), mb, "")) { ui.open_popup(mpid); for (int i = 0; i < ai::kProviderCount; i++) agent_load_models(i); }
    r.draw_text_in(FontId::Regular, 11, {mb.x + 8, mb.y, mb.w - 28, mb.h}, r.ellipsize(FontId::Regular, 11, model, mb.w - 28), T.text);
    r.icon("triangle-down", mb.right() - 20, mb.y + 4, 16, T.text_muted);
    if (ui::icon_button(ui, ui.id("ai-new"), {head.right() - 60, head.y + 5, 24, 23}, "plus", "New chat") && !agent.running) {
        agent.history.clear();
        agent.items.clear();
        agent.allow_all = false;
    }
    if (ui::icon_button(ui, ui.id("ai-close"), {head.right() - 32, head.y + 5, 24, 23}, "x", "Close (Ctrl+L)")) agent.open = false;
    // model popup: providers, then that provider's models
    Rect pp{mb.right() - 320, mb.bottom() + 4, 320, std::min(460.f, rc.h - 60)};
    if (ui::begin_popup(ui, mpid, pp)) {
        float y = pp.y + 8;
        for (int i = 0; i < ai::kProviderCount; i++) {
            if (ui::radio(ui, ui.id("ai-prov", i), {pp.x + 12, y, pp.w - 24, 24}, p == i, ai::provider_name(ai::Provider(i)))) {
                settings.ai_provider = i;
                save_state();
                agent_load_models(i);
            }
            y += 26;
        }
        r.line_h(pp.x, pp.right(), y + 4, T.border);
        y += 10;
        int cp = settings.ai_provider;
        bool need_key = ai::needs_key(ai::Provider(cp)) && ai_key(cp).empty();
        Rect list{pp.x, y, pp.w, pp.bottom() - y - 4};
        if (need_key) {
            ui::wrapped_text(ui, list.inset(12, 6), std::string("Add your ") + ai::provider_name(ai::Provider(cp)) + " API key in Settings → AI to list models.",
                             FontId::Regular, 12, T.text_muted, 1.4f);
        } else if (agent.models_loading.count(cp)) {
            ui::spinner(ui, list.x + list.w / 2, list.y + 30, 7, T.text_muted);
        } else if (!agent.models_error[cp].empty()) {
            ui::wrapped_text(ui, list.inset(12, 6), agent.models_error[cp], FontId::Regular, 12, T.danger, 1.4f);
        } else {
            float off = ui::begin_scroll(ui, ui.id("ai-model-list"), list);
            float yy = list.y - off;
            int i = 0;
            for (auto& m : agent.models[cp]) {
                if (ui::menu_item(ui, ui.id("ai-m", i++), {list.x, yy, list.w, 26}, m, "", true, "", settings.ai_model[cp] == m)) {
                    settings.ai_model[cp] = m;
                    save_state();
                }
                yy += 26;
            }
            ui::end_scroll(ui, ui.id("ai-model-list"), list, yy + off - list.y);
        }
        ui::end_popup(ui);
    }

    // footer: approval card + input
    float approval_h = 0;
    if (agent.approval) approval_h = 80 + (agent.approval->preview.empty() ? 0 : std::min(200.f, 16.f * float(std::count(agent.approval->preview.begin(), agent.approval->preview.end(), '\n') - 1)));
    Rect foot{rc.x + 1, rc.bottom() - 112 - approval_h, rc.w - 1, 112 + approval_h};
    r.line_h(foot.x, foot.right(), foot.y, T.border);
    float fy = foot.y + 10;
    if (agent.approval) {
        auto& ap = *agent.approval;
        Rect card{foot.x + 10, fy, foot.w - 20, approval_h - 10};
        r.rect(card, T.attention_subtle, 6, 1, T.dark ? Color::hex(0x9e6a03) : Color::hex(0xd4a72c));
        r.draw_text_in(FontId::Bold, 12, {card.x + 10, card.y + 6, card.w - 20, 18}, ap.title + "?", T.text);
        if (!ap.preview.empty()) {
            size_t pos = ap.preview.find('\n');
            std::string stat = ap.preview.substr(0, pos);
            pos = pos == std::string::npos ? ap.preview.size() : pos + 1;
            float tw = ui.text_w(FontId::Bold, 12, ap.title + "?");
            r.draw_text_in(FontId::Regular, 11, {card.x + 18 + tw, card.y + 6, 120, 18}, stat, T.text_muted);
            float py = card.y + 28;
            while (pos < ap.preview.size() && py + 16 <= card.bottom() - 36) {
                size_t nl = ap.preview.find('\n', pos);
                std::string l = ap.preview.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                Color col = !l.empty() && l[0] == '+' ? T.success : !l.empty() && l[0] == '-' ? T.danger : T.text_muted;
                r.draw_text_in(FontId::Mono, 11, {card.x + 10, py, card.w - 20, 16}, r.ellipsize(FontId::Mono, 11, l, card.w - 20), col);
                py += 16;
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
        float bx = card.right() - 10;
        auto answer = [&](int v) { auto a = ap.answer; agent.approval.reset(); a->set_value(v); };
        if (ui::button(ui, ui.id("ai-allow"), {bx - 80, card.bottom() - 32, 80, 24}, "Allow", ui::BtnStyle::Primary)) answer(1);
        else if (ui::button(ui, ui.id("ai-allow-all"), {bx - 200, card.bottom() - 32, 114, 24}, "Allow all in chat")) answer(2);
        else if (ui::button(ui, ui.id("ai-reject"), {bx - 286, card.bottom() - 32, 80, 24}, "Decline")) answer(0);
        fy += approval_h;
    }
    Rect input{foot.x + 10, fy, foot.w - 20, 62};
    ui::TextFieldOpts io;
    io.multiline = true;
    io.placeholder = agent.running ? "Working…" : "Ask the assistant to change or explain code";
    auto iid = ui.id("ai-input");
    bool send = false;
    if (ui.has_focus(iid) && ui.key_pressed(Key::Enter, 0)) { ui.take_key(Key::Enter, 0); send = true; }
    ui::text_field(ui, iid, input, agent.input, io);
    float by = input.bottom() + 8;
    r.draw_text_in(FontId::Regular, 11, {foot.x + 10, by, foot.w - 120, 24},
                   settings.ai_ask ? "Asks before changing files" : "Changes files without asking", T.text_muted);
    if (agent.running) {
        if (ui::button(ui, ui.id("ai-stop"), {foot.right() - 90, by, 80, 24}, "Stop")) { if (agent.cancel) agent.cancel->store(true); }
    } else if (ui::button(ui, ui.id("ai-send"), {foot.right() - 90, by, 80, 24}, "Send", ui::BtnStyle::Primary, !git::trim(agent.input).empty()) || send) {
        std::string t = agent.input;
        agent.input.clear();
        agent_send(t);
    }

    // messages
    Rect list{rc.x + 1, head.bottom(), rc.w - 1, foot.y - head.bottom()};
    auto lid = ui.id("ai-list");
    float w = list.w - 28;
    // measure
    std::vector<float> hs;
    float total = 12;
    for (auto& it : agent.items) {
        float h = 0;
        if (it.kind == AgentItem::Tool) h = 24;
        else if (it.kind == AgentItem::Error) h = ui::wrapped_text(ui, {0, 0, w - 24, 2000}, it.text, FontId::Regular, 12, T.text, 1.4f, true) + 14;
        else h = 22 + (it.text.empty() ? 18 : draw_markdown(*this, {0, 0, w, 0}, it.text, true));
        hs.push_back(h);
        total += h + 8;
    }
    if (agent.scroll_to_bottom > 0) {
        ui::scroll_to_visible(ui, lid, total, 1, list.h);
        agent.scroll_to_bottom = 0;
    }
    float off = ui::begin_scroll(ui, lid, list);
    float y = list.y + 12 - off;
    if (agent.items.empty()) {
        float cy = list.y + list.h / 2 - 60;
        r.icon("sparkle-fill", list.x + list.w / 2 - 12, cy, 24, T.accent_emphasis);
        r.draw_text_in(FontId::Bold, 13, {list.x, cy + 34, list.w, 20}, "Ask about " + ide.repo, T.text, Align::Center);
        ui::wrapped_text(ui, {list.x + 30, cy + 58, list.w - 60, 80},
                         "The assistant can read, search and edit this repository's files. Its edits join yours in the Changes tab and go out in the same pull request.",
                         FontId::Regular, 12, T.text_muted, 1.5f);
    }
    for (size_t i = 0; i < agent.items.size(); i++) {
        auto& it = agent.items[i];
        float h = hs[i];
        if (y + h >= list.y && y <= list.bottom()) {
            float x = list.x + 14;
            if (it.kind == AgentItem::User || it.kind == AgentItem::Assistant) {
                bool user = it.kind == AgentItem::User;
                if (user) draw_avatar({x, y, 16, 16}, account.name, account.email);
                else r.icon("sparkle-fill", x, y, 16, T.accent_emphasis);
                r.draw_text_in(FontId::Bold, 12, {x + 22, y - 1, w, 18}, user ? "You" : settings.ai_model[p], T.text);
                if (it.text.empty() && agent.running && i + 1 == agent.items.size()) ui::spinner(ui, x + 6, y + 30, 5, T.text_muted);
                else draw_markdown(*this, {x, y + 22, w, h}, it.text, false);
            } else if (it.kind == AgentItem::Tool) {
                Rect row{x, y, w, 22};
                bool hov = !it.target.empty() && ui.hovered(row);
                if (hov) { r.rect(row, T.row_hover, 5); ui.set_cursor(plat::Cursor::Hand); }
                const char* ic = !it.ok ? "x" : it.text.rfind("Edited", 0) == 0 || it.text.rfind("Rewrote", 0) == 0 ? "pencil"
                                 : it.text.rfind("Created", 0) == 0 ? "plus" : it.text.rfind("Deleted", 0) == 0 ? "trash"
                                 : it.text.rfind("Searched", 0) == 0 ? "search" : "file";
                r.icon(ic, row.x + 4, row.y + 3, 16, it.ok ? T.text_muted : T.danger);
                r.draw_text_in(FontId::Regular, 12, {row.x + 26, row.y, row.w - 30, row.h}, r.ellipsize(FontId::Regular, 12, it.text, row.w - 30),
                               it.ok ? T.text_muted : T.danger);
                if (!it.target.empty() && ui.clicked(ui.id("ai-tool", int64_t(i)), row) && ide.files.count(it.target) && !ide.files[it.target].deleted) {
                    ide.side_tab = 0;
                    ide_open_file(it.target);
                }
            } else {
                Rect eb{x, y, w, h};
                r.rect(eb, T.danger_subtle, 6);
                r.icon("alert", eb.x + 8, eb.y + 7, 16, T.danger);
                ui::wrapped_text(ui, {eb.x + 30, eb.y + 7, eb.w - 38, h}, it.text, FontId::Regular, 12, T.text, 1.4f);
            }
        }
        y += h + 8;
    }
    ui::end_scroll(ui, lid, list, total);
}

}  // namespace app
