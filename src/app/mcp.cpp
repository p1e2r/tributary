// SPDX-License-Identifier: MIT
// `Tributary --mcp`: a Model Context Protocol server over stdio. It exposes Tributary's git
// engine and GitHub account to AI clients (Claude Desktop, Claude Code, VS Code, Cursor…):
// status, diffs, history, branches, commits, fetch/pull/push and pull requests, using the
// repositories and the GitHub sign-in already set up in Tributary.
#include "mcp.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../git/lfs.hpp"
#include "../git/repo.hpp"
#include "../git/transport.hpp"
#include "../net/github.hpp"
#include "../net/http.hpp"
#include "../net/json.hpp"
#include "../platform/platform.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace app {
namespace {

using json::Array;
using json::Object;
using json::Value;

constexpr const char* kVersion = TRIBUTARY_VERSION;

struct Ctx {
    std::string default_repo;                // --repo, the working directory, or Tributary's selected repo
    std::vector<std::pair<std::string, std::string>> repos;  // name, path (Tributary's list)
    gh::Account account, enterprise;
};

struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void load_tributary_state(Ctx& c) {
    auto data = git::try_read_file(plat::app_data_dir() + "/state.json");
    if (data) {
        try {
            auto v = json::parse(*data);
            for (auto& r : v["repositories"].arr()) {
                std::string path = r["path"].str();
                if (path.empty()) continue;
                std::string name = r["name"].str(git::path_to_git(fs::path(git::git_to_path(path)).filename()));
                if (!r["owner"].str().empty()) name = r["owner"].str() + "/" + name;
                c.repos.push_back({name, path});
            }
            int sel = int(v["selected"].integer(-1));
            if (sel >= 0 && size_t(sel) < c.repos.size()) c.default_repo = c.repos[size_t(sel)].second;
            auto acc = [](const Value& a, const char* prefix) {
                gh::Account r;
                r.login = a["login"].str();
                r.name = a["name"].str();
                r.email = a["email"].str();
                r.endpoint = a["endpoint"].str("https://api.github.com");
                if (!r.login.empty()) r.token = plat::secret_get(std::string(prefix) + r.login);
                return r;
            };
            c.account = acc(v["account"], "github:");
            auto& g = v["enterprise"];
            if (!g["login"].str().empty()) {
                c.enterprise = acc(g, "");
                c.enterprise.token = plat::secret_get("ghe:" + c.enterprise.host() + ":" + c.enterprise.login);
            }
        } catch (...) {}
    }
    if (const char* t = std::getenv("GITHUB_TOKEN"); t && *t && !c.account.valid()) c.account.token = t;
}

git::CredentialProvider credentials(const Ctx& c) {
    gh::Account acc = c.account, ghe = c.enterprise;
    return [acc, ghe](const std::string& url, int attempt) -> std::optional<git::Credentials> {
        if (attempt > 0) return std::nullopt;  // no one to ask
        auto host = net::Url::parse(url).host;
        if (acc.valid() && host == "github.com") return git::Credentials{acc.login.empty() ? "x-access-token" : acc.login, acc.token};
        if (ghe.valid() && host == ghe.host()) return git::Credentials{ghe.login, ghe.token};
        return std::nullopt;
    };
}

git::Repository open_repo(const Ctx& c, const Value& args) {
    std::string want = args["repository"].str();
    std::string path = want.empty() ? c.default_repo : want;
    // allow a repository's name from Tributary's list as well as a path
    for (auto& [name, p] : c.repos)
        if (!want.empty() && (want == name || name.size() > want.size() && name.compare(name.size() - want.size() - 1, std::string::npos, "/" + want) == 0)) path = p;
    if (path.empty()) throw ToolError("No repository given. Pass `repository` (a path, or a name from list_repositories).");
    auto wt = git::Repository::discover(git::git_to_path(path));
    if (!wt) throw ToolError(path + " isn't a Git repository.");
    return git::Repository::open(*wt);
}

std::pair<gh::Account, std::pair<std::string, std::string>> github_repo(const Ctx& c, git::Repository& repo, const std::string& remote = "origin") {
    auto url = repo.config().get("remote." + remote + ".url");
    if (!url) throw ToolError("The repository has no remote named " + remote + ".");
    if (auto gr = gh::parse_github_remote(*url)) {
        if (!c.account.valid()) throw ToolError("Sign in to GitHub in Tributary (or set GITHUB_TOKEN) to use pull requests.");
        return {c.account, *gr};
    }
    if (c.enterprise.valid())
        if (auto gr = gh::parse_github_remote(*url, c.enterprise.host())) return {c.enterprise, *gr};
    throw ToolError(*url + " isn't a GitHub repository.");
}

std::string status_text(git::Repository& repo) {
    auto st = repo.status();
    std::string out = "On branch " + (repo.current_branch().empty() ? std::string("(detached HEAD)") : repo.current_branch());
    if (auto up = repo.upstream_of(repo.current_branch()); !up.empty()) {
        auto h = repo.head();
        auto u = repo.refs().resolve(up);
        if (h && u) {
            auto [a, b] = repo.ahead_behind(*h, *u);
            out += " (" + up.substr(std::string("refs/remotes/").size()) + ": " + std::to_string(a) + " ahead, " + std::to_string(b) + " behind)";
        }
    }
    out += "\n";
    if (st.merging) out += "A merge is in progress.\n";
    if (st.rewrite) out += "A rebase is stopped on conflicts.\n";
    if (st.files.empty()) return out + "No changes.\n";
    for (auto& f : st.files) {
        out += std::string(f.conflicted ? "conflicted" : git::change_name(f.change)) + "  " + f.path;
        if (!f.old_path.empty()) out += " (from " + f.old_path + ")";
        out += "\n";
    }
    return out;
}

std::string date(int64_t t) {
    std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::vector<std::string> str_list(const Value& v) {
    std::vector<std::string> out;
    for (auto& s : v.arr()) out.push_back(s.str());
    return out;
}

// ---------------------------------------------------------------- tool table
struct Tool {
    const char* name;
    const char* title;
    const char* description;
    Object props;
    Array required;
    bool read_only, destructive;
    std::function<std::string(Ctx&, const Value&)> run;
};

Value S(const char* d) { return Object{{"type", "string"}, {"description", d}}; }
Value I(const char* d) { return Object{{"type", "integer"}, {"description", d}}; }
Value B(const char* d) { return Object{{"type", "boolean"}, {"description", d}}; }
Value L(const char* d) { return Object{{"type", "array"}, {"items", Object{{"type", "string"}}}, {"description", d}}; }
const char* kRepoDesc = "Repository path or name (see list_repositories). Defaults to the current one.";

std::vector<Tool> tools() {
    std::vector<Tool> t;
    t.push_back({"list_repositories", "List repositories", "Repositories added to Tributary, with their paths and current branch.", {}, {}, true, false,
                 [](Ctx& c, const Value&) {
                     std::string out;
                     for (auto& [name, path] : c.repos) {
                         out += name + "  " + path;
                         try {
                             auto r = git::Repository::open(git::git_to_path(path));
                             out += "  [" + r.current_branch() + "]";
                         } catch (...) { out += "  (missing)"; }
                         if (path == c.default_repo) out += "  (current)";
                         out += "\n";
                     }
                     return out.empty() ? std::string("No repositories have been added to Tributary.") : out;
                 }});
    t.push_back({"status", "Working tree status", "The current branch, how far it is from its upstream, and the changed files.",
                 {{"repository", S(kRepoDesc)}}, {}, true, false, [](Ctx& c, const Value& a) { auto r = open_repo(c, a); return status_text(r); }});
    t.push_back({"diff", "Diff of uncommitted changes", "Unified diff of uncommitted changes, for all files or one path.",
                 {{"repository", S(kRepoDesc)}, {"path", S("Only this file (optional)")}}, {}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string path = a["path"].str(), out;
                     for (auto& f : r.status().files)
                         if (path.empty() || f.path == path) out += git::unified_patch(f.path, r.working_diff(f));
                     return out.empty() ? std::string("No changes.") : out;
                 }});
    t.push_back({"log", "Commit history", "Recent commits on a branch or revision (default HEAD).",
                 {{"repository", S(kRepoDesc)}, {"revision", S("Branch, tag or commit (optional)")}, {"count", I("How many (default 20)")}, {"path", S("Only commits touching this path")}},
                 {}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string rev = a["revision"].str("HEAD");
                     auto id = r.rev_parse(rev.empty() ? "HEAD" : rev);
                     if (!id) throw ToolError("Unknown revision " + rev);
                     std::string out;
                     for (auto& ci : r.log(*id, size_t(std::clamp<int64_t>(a["count"].integer(20), 1, 500)), 0, a["path"].str())) {
                         auto& au = ci.commit.author;
                         out += ci.id.short_hex() + "  " + ci.commit.summary() + "  — " + au.name + ", " + date(au.time) + "\n";
                     }
                     return out.empty() ? std::string("No commits.") : out;
                 }});
    t.push_back({"show_commit", "Show a commit", "A commit's message and full diff.",
                 {{"repository", S(kRepoDesc)}, {"revision", S("Commit, branch or tag")}}, {"revision"}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto id = r.rev_parse(a["revision"].str());
                     if (!id) throw ToolError("Unknown revision " + a["revision"].str());
                     auto ci = r.log(*id, 1);
                     std::string out;
                     if (!ci.empty()) {
                         auto& cm = ci[0].commit;
                         out = "commit " + id->hex() + "\nAuthor: " + cm.author.name + " <" + cm.author.email + ">\nDate:   " + date(cm.author.time) + " UTC\n\n" + cm.message + "\n";
                     }
                     for (auto& f : r.commit_changes(*id)) out += git::unified_patch(f.path, r.commit_file_diff(f));
                     return out;
                 }});
    t.push_back({"read_file", "Read a file", "A file's contents in the working tree, or at a revision.",
                 {{"repository", S(kRepoDesc)}, {"path", S("Path relative to the repository root")}, {"revision", S("Read it at this commit/branch instead (optional)")}},
                 {"path"}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string path = a["path"].str(), rev = a["revision"].str();
                     std::string data;
                     if (rev.empty()) {
                         auto d = git::try_read_file(r.worktree() / git::git_to_path(path));
                         if (!d) throw ToolError("There's no file at " + path + ".");
                         data = *d;
                     } else {
                         auto id = r.rev_parse(rev);
                         if (!id) throw ToolError("Unknown revision " + rev);
                         auto cm = git::Commit::parse(r.blob(*id));
                         auto e = r.tree_entry(cm.tree, path);
                         if (!e) throw ToolError(path + " doesn't exist at " + rev + ".");
                         data = r.blob(e->id);
                     }
                     if (data.find('\0') != std::string::npos) return std::string("(binary file, ") + std::to_string(data.size()) + " bytes)";
                     return data;
                 }});
    t.push_back({"branches", "List branches", "Local and remote-tracking branches, with ahead/behind counts.",
                 {{"repository", S(kRepoDesc)}}, {}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string out;
                     for (auto& b : r.branches())
                         out += std::string(b.current ? "* " : "  ") + b.name + (b.remote ? "" : "  +" + std::to_string(b.ahead) + " -" + std::to_string(b.behind)) + "\n";
                     return out;
                 }});
    t.push_back({"create_branch", "Create a branch", "Create a branch from HEAD (or a start point) and switch to it.",
                 {{"repository", S(kRepoDesc)}, {"name", S("New branch name")}, {"start", S("Start point (optional)")}, {"checkout", B("Switch to it (default true)")}},
                 {"name"}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::optional<git::ObjectId> start;
                     if (!a["start"].str().empty()) {
                         start = r.rev_parse(a["start"].str());
                         if (!start) throw ToolError("Unknown revision " + a["start"].str());
                     }
                     bool co = a["checkout"].boolean(true);
                     r.create_branch(a["name"].str(), start, co);
                     return "Created " + a["name"].str() + (co ? " and switched to it." : ".");
                 }});
    t.push_back({"checkout", "Switch branch", "Switch to a branch (a remote branch like origin/x creates a tracking branch). Refuses to overwrite local changes.",
                 {{"repository", S(kRepoDesc)}, {"branch", S("Branch name")}}, {"branch"}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     r.checkout_branch(a["branch"].str());
                     return "Switched to " + r.current_branch() + ".";
                 }});
    t.push_back({"commit", "Commit changes", "Commit changed files on the current branch. Runs hooks and signs if the repository is set up to.",
                 {{"repository", S(kRepoDesc)}, {"message", S("Summary line")}, {"description", S("Body (optional)")},
                  {"paths", L("Only these files (default: every changed file)")}},
                 {"message"}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::vector<git::CommitSelection> sel;
                     auto only = str_list(a["paths"]);
                     for (auto& f : r.status().files)
                         if (only.empty() || std::find(only.begin(), only.end(), f.path) != only.end()) sel.push_back({f.path, std::nullopt});
                     if (sel.empty()) throw ToolError("Nothing to commit.");
                     auto id = r.commit(sel, a["message"].str(), a["description"].str());
                     return "Committed " + std::to_string(sel.size()) + " file" + (sel.size() == 1 ? "" : "s") + " as " + id.short_hex() + " on " + r.current_branch() + ".";
                 }});
    t.push_back({"discard_changes", "Discard changes", "Throw away uncommitted changes to the given files. Cannot be undone.",
                 {{"repository", S(kRepoDesc)}, {"paths", L("Files to restore")}}, {"paths"}, false, true,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto p = str_list(a["paths"]);
                     r.discard(p);
                     return "Discarded changes to " + std::to_string(p.size()) + " file(s).";
                 }});
    t.push_back({"fetch", "Fetch", "Fetch from a remote.", {{"repository", S(kRepoDesc)}, {"remote", S("Remote (default origin)")}}, {}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string rem = a["remote"].str("origin");
                     git::fetch(r, rem.empty() ? "origin" : rem, credentials(c), {});
                     return "Fetched " + rem + ".\n\n" + status_text(r);
                 }});
    t.push_back({"pull", "Pull", "Fetch and merge the current branch's upstream.", {{"repository", S(kRepoDesc)}}, {}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto m = git::pull(r, credentials(c), {});
                     const char* k[] = {"Already up to date.", "Fast-forwarded.", "Merged.", "Merge stopped on conflicts:"};
                     std::string out = k[m.kind];
                     for (auto& f : m.conflicts) out += "\n  " + f;
                     return out;
                 }});
    t.push_back({"push", "Push", "Push the current branch (or another) to a remote, setting its upstream if it has none.",
                 {{"repository", S(kRepoDesc)}, {"remote", S("Remote (default origin)")}, {"branch", S("Branch (default current)")},
                  {"force", B("Force-push with lease (rewrites the remote branch)")}},
                 {}, false, true,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     std::string rem = a["remote"].str("origin"), br = a["branch"].str(r.current_branch());
                     if (rem.empty()) rem = "origin";
                     if (br.empty()) throw ToolError("HEAD is detached; pass a branch.");
                     bool set_up = r.upstream_of(br).empty();
                     git::push(r, rem, br, credentials(c), {}, set_up, a["force"].boolean(false));
                     return "Pushed " + br + " to " + rem + ".";
                 }});
    t.push_back({"merge", "Merge a branch", "Merge a branch into the current one.",
                 {{"repository", S(kRepoDesc)}, {"branch", S("Branch to merge in")}, {"squash", B("Squash into one change (commit it afterwards)")}},
                 {"branch"}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto m = r.merge(a["branch"].str(), a["squash"].boolean(false));
                     const char* k[] = {"Already up to date.", "Fast-forwarded.", "Merged.", "Merge stopped on conflicts:"};
                     std::string out = k[m.kind];
                     for (auto& f : m.conflicts) out += "\n  " + f;
                     return out;
                 }});
    t.push_back({"stash", "Stash changes", "Stash all uncommitted changes.", {{"repository", S(kRepoDesc)}, {"message", S("Message (optional)")}}, {}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     r.stash_push(a["message"].str());
                     return std::string("Stashed.");
                 }});
    t.push_back({"stash_pop", "Restore stash", "Apply and drop the latest stash.", {{"repository", S(kRepoDesc)}}, {}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     r.stash_apply(0, true);
                     return "Restored the stash.\n\n" + status_text(r);
                 }});
    t.push_back({"list_pull_requests", "List pull requests", "Open pull requests on the repository's GitHub remote.",
                 {{"repository", S(kRepoDesc)}}, {}, true, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto [acc, gr] = github_repo(c, r);
                     std::string out;
                     for (auto& p : gh::list_pull_requests(acc, gr.first, gr.second))
                         out += "#" + std::to_string(p.number) + "  " + p.title + "  (" + p.head_ref + " → " + p.base_ref + ", @" + p.author + (p.draft ? ", draft" : "") + ")\n";
                     return out.empty() ? std::string("No open pull requests.") : out;
                 }});
    t.push_back({"create_pull_request", "Create a pull request", "Open a pull request on GitHub from a pushed branch (default: the current branch into the default branch).",
                 {{"repository", S(kRepoDesc)}, {"title", S("Title")}, {"body", S("Description (markdown)")}, {"head", S("Branch with the changes (default current)")},
                  {"base", S("Branch to merge into (default: the repository's default branch)")}, {"draft", B("Open as a draft")}},
                 {"title"}, false, false,
                 [](Ctx& c, const Value& a) {
                     auto r = open_repo(c, a);
                     auto [acc, gr] = github_repo(c, r);
                     std::string head = a["head"].str(r.current_branch()), base = a["base"].str();
                     if (base.empty()) base = gh::get_repo(acc, gr.first, gr.second).default_branch;
                     Object body{{"title", a["title"].str()}, {"body", a["body"].str()}, {"head", head}, {"base", base}, {"draft", a["draft"].boolean(false)}};
                     auto v = gh::rest(acc, "POST", "/repos/" + gr.first + "/" + gr.second + "/pulls", Value(body).dump());
                     return "Opened #" + std::to_string(v["number"].integer()) + ": " + v["html_url"].str();
                 }});
    return t;
}

Value tool_schema(const Tool& t) {
    Object o{{"name", t.name}, {"title", t.title}, {"description", t.description},
             {"inputSchema", Object{{"type", "object"}, {"properties", t.props}, {"required", t.required}}},
             {"annotations", Object{{"title", t.title}, {"readOnlyHint", t.read_only}, {"destructiveHint", t.destructive}, {"openWorldHint", false}}}};
    return o;
}

void send(const Value& v) {
    std::string s = v.dump();
    s += "\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int mcp_main(const std::string& repo_arg) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Ctx ctx;
    load_tributary_state(ctx);
    if (!repo_arg.empty()) ctx.default_repo = repo_arg;
    else if (auto wt = git::Repository::discover(fs::current_path())) ctx.default_repo = git::path_to_git(*wt);
    auto list = tools();
    std::fprintf(stderr, "tributary mcp: %zu tools, %zu repositories, default %s\n", list.size(), ctx.repos.size(),
                 ctx.default_repo.empty() ? "(none)" : ctx.default_repo.c_str());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        Value msg;
        try { msg = json::parse(line); } catch (...) {
            send(Object{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", Object{{"code", -32700}, {"message", "Parse error"}}}});
            continue;
        }
        auto handle = [&](const Value& m) -> std::optional<Value> {
            const Value& id = m["id"];
            bool is_request = m.is_object() && m.obj().count("id") && !id.is_null();
            std::string method = m["method"].str();
            auto ok = [&](Value result) { return Value(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); };
            auto err = [&](int code, const std::string& text) {
                return Value(Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", Object{{"code", code}, {"message", text}}}});
            };
            if (!is_request) return std::nullopt;  // notifications (initialized, cancelled…) and responses need no reply
            if (method == "initialize") {
                std::string want = m["params"]["protocolVersion"].str();
                std::string ver = (want == "2025-06-18" || want == "2025-03-26" || want == "2024-11-05") ? want : "2025-06-18";
                return ok(Object{{"protocolVersion", ver},
                                 {"capabilities", Object{{"tools", Object{{"listChanged", false}}}}},
                                 {"serverInfo", Object{{"name", "tributary"}, {"title", "Tributary"}, {"version", kVersion}}},
                                 {"instructions", "Git and GitHub tools backed by Tributary. Omit `repository` to use the current one; "
                                                  "list_repositories shows what's available."}});
            }
            if (method == "ping") return ok(Object{});
            if (method == "tools/list") {
                Array a;
                for (auto& t : list) a.push_back(tool_schema(t));
                return ok(Object{{"tools", a}});
            }
            if (method == "tools/call") {
                std::string name = m["params"]["name"].str();
                const Value& args = m["params"]["arguments"];
                for (auto& t : list) {
                    if (name != t.name) continue;
                    std::string text;
                    bool is_error = false;
                    try {
                        text = t.run(ctx, args);
                    } catch (const std::exception& e) {
                        text = e.what();
                        is_error = true;
                    }
                    if (text.size() > 400000) text = text.substr(0, 400000) + "\n… (truncated)";
                    return ok(Object{{"content", Array{Object{{"type", "text"}, {"text", text}}}}, {"isError", is_error}});
                }
                return err(-32602, "Unknown tool: " + name);
            }
            if (method == "resources/list") return ok(Object{{"resources", Array{}}});
            if (method == "prompts/list") return ok(Object{{"prompts", Array{}}});
            return err(-32601, "Method not found: " + method);
        };
        if (msg.is_array()) {  // JSON-RPC batch (2025-03-26)
            Array out;
            for (auto& m : msg.arr())
                if (auto r = handle(m)) out.push_back(*r);
            if (!out.empty()) send(out);
        } else if (auto r = handle(msg)) {
            send(*r);
        }
    }
    return 0;
}

}  // namespace app
