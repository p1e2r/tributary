// SPDX-License-Identifier: MIT
// Developer CLI over the Tributary git engine (used by the test suite).
#include <cstdio>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <iostream>

#include "../src/git/repo.hpp"
#include "../src/git/transport.hpp"
#include "../src/git/lfs.hpp"
#include "../src/git/archive.hpp"
#include "../src/net/ai.hpp"

using namespace git;

static void usage() {
    std::puts("tribcli <cmd> [...]\n"
              "  init <dir> | status | commit <msg> [paths...] | log [n] | branch [name] | checkout <name>\n"
              "  merge <ref> | stash [msg] | stash-pop | stash-list | diff <path> | show <rev>\n"
              "  revert <rev> | undo | catfile <id> | clone <url> <dir> | fetch [remote] | push [remote] [branch]\n"
              "  pull | partial <path> <idx,idx,...> <msg> | rename <from> <to> | delete <branch>\n"
              "  lfs-track <pattern> | lfs-pull [remote] | is-lfs <path>\n"
              "  ai <anthropic|openai|gemini|ollama> <model> <prompt> [base-url]   (key from TRIBUTARY_AI_KEY)\n"
              "  ai-models <provider> [base-url]\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // byte-exact output like git's
#endif
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    try {
        if (cmd == "init") { Repository::init(argv[2]); return 0; }
        if (cmd == "extract") {
            for (auto& t : extract_archive(read_file(argv[2]), argv[3])) std::puts(t.c_str());
            return 0;
        }
        if (cmd == "ai" || cmd == "ai-models") {
            // Exercise the assistant's provider layer: a tool loop over files in the current directory.
            auto prov = [](const std::string& s) {
                for (int i = 0; i < ai::kProviderCount; i++)
                    if (s == ai::provider_short(ai::Provider(i))) return ai::Provider(i);
                throw std::runtime_error("unknown provider " + s);
            };
            ai::Config c;
            c.provider = prov(argv[2]);
            const char* key = std::getenv("TRIBUTARY_AI_KEY");
            c.key = key ? key : "";
            if (cmd == "ai-models") {
                if (argc > 3) c.base_url = argv[3];
                for (auto& m : ai::list_models(c)) std::puts(m.c_str());
                return 0;
            }
            if (argc < 5) { usage(); return 1; }
            c.model = argv[3];
            if (argc > 5) c.base_url = argv[5];
            auto obj = [](json::Object props, json::Array req) { return json::Value(json::Object{{"type", "object"}, {"properties", props}, {"required", req}}); };
            auto str = json::Value(json::Object{{"type", "string"}});
            std::vector<ai::ToolDef> tools = {
                {"read_file", "Read a file", obj({{"path", str}}, {"path"})},
                {"edit_file", "Replace old_string with new_string in a file", obj({{"path", str}, {"old_string", str}, {"new_string", str}}, {"path", "old_string", "new_string"})},
            };
            std::vector<ai::Message> h{{ai::Message::User, argv[4], {}, {}, {}}};
            for (int step = 0; step < 10; step++) {
                auto t = ai::complete(c, "You edit files in the current directory.", h, tools,
                                      [](const std::string& s) { std::fputs(s.c_str(), stdout); std::fflush(stdout); }, [] { return true; });
                if (!t.error.empty()) throw std::runtime_error(t.error);
                h.push_back({ai::Message::Assistant, t.text, t.calls, {}, t.raw});
                std::printf("\n[stop %s]\n", t.stop_reason.c_str());
                if (t.calls.empty()) break;
                ai::Message res;
                res.role = ai::Message::Tools;
                for (auto& tc : t.calls) {
                    std::printf("[tool %s %s]\n", tc.name.c_str(), tc.input.dump().c_str());
                    ai::ToolResult r{tc.id, tc.name, "", false};
                    std::string path = tc.input["path"].str();
                    std::string data = read_file(path);
                    if (tc.name == "read_file") r.content = data;
                    else {
                        auto pos = data.find(tc.input["old_string"].str());
                        if (pos == std::string::npos) { r.content = "not found"; r.error = true; }
                        else {
                            data.replace(pos, tc.input["old_string"].str().size(), tc.input["new_string"].str());
                            write_file(path, data);
                            r.content = "ok";
                        }
                    }
                    res.results.push_back(r);
                }
                h.push_back(res);
            }
            return 0;
        }
        if (cmd == "clone") {
            auto r = clone(argv[2], argv[3], {}, [](const Progress& p) {
                std::fprintf(stderr, "\r%s %zu/%zu      ", p.phase.c_str(), p.done, p.total);
            });
            std::fprintf(stderr, "\n");
            return 0;
        }
        auto repo = Repository::open(fs::current_path());
        if (cmd == "status") {
            auto st = repo.status();
            for (auto& f : st.files)
                std::printf("%-10s %s%s%s\n", change_name(f.change), f.path.c_str(),
                            f.old_path.empty() ? "" : " <- ", f.old_path.c_str());
            if (st.merging) std::puts("(merging)");
        } else if (cmd == "commit") {
            std::vector<CommitSelection> sel;
            if (argc > 3) for (int i = 3; i < argc; i++) sel.push_back({argv[i], std::nullopt});
            else for (auto& f : repo.status().files) sel.push_back({f.path, std::nullopt});
            auto id = repo.commit(sel, argv[2], "");
            std::puts(id.hex().c_str());
        } else if (cmd == "partial") {
            std::vector<bool> lines(10000, false);
            std::string s = argv[3];
            size_t p = 0;
            while (p < s.size()) { size_t c = s.find(',', p); lines[std::stoi(s.substr(p, c - p))] = true; if (c == std::string::npos) break; p = c + 1; }
            std::puts(repo.commit({{argv[2], lines}}, argv[4], "").hex().c_str());
        } else if (cmd == "log") {
            auto h = repo.head();
            if (!h) return 0;
            for (auto& c : repo.log(*h, argc > 2 ? std::stoul(argv[2]) : 20))
                std::printf("%s %s\n", c.id.short_hex().c_str(), c.commit.summary().c_str());
        } else if (cmd == "branch") {
            if (argc > 2) repo.create_branch(argv[2], std::nullopt, false);
            else for (auto& b : repo.branches())
                std::printf("%c %s%s +%d -%d\n", b.current ? '*' : ' ', b.name.c_str(), b.remote ? " (remote)" : "", b.ahead, b.behind);
        } else if (cmd == "checkout") {
            repo.checkout_branch(argv[2]);
        } else if (cmd == "merge") {
            auto m = repo.merge(argv[2]);
            const char* k[] = {"up-to-date", "fast-forward", "merged", "conflicts"};
            std::printf("%s\n", k[m.kind]);
            for (auto& c : m.conflicts) std::printf("  conflict: %s\n", c.c_str());
        } else if (cmd == "stash") {
            std::puts(repo.stash_push(argc > 2 ? argv[2] : "").hex().c_str());
        } else if (cmd == "stash-pop") {
            repo.stash_apply(0, true);
        } else if (cmd == "stash-list") {
            for (auto& s : repo.stashes()) std::printf("stash@{%d}: %s\n", s.index, s.message.c_str());
        } else if (cmd == "diff") {
            for (auto& f : repo.status().files)
                if (f.path == argv[2]) std::fputs(unified_patch(f.path, repo.working_diff(f)).c_str(), stdout);
        } else if (cmd == "fulldiff") {
            auto lines = full_diff_lines(repo.head_content(argv[2]), repo.worktree_content(argv[2]));
            for (auto& l : lines) std::printf("%d %c %s\n", l.index, l.kind == LineKind::Added ? '+' : l.kind == LineKind::Deleted ? '-' : ' ', l.text.c_str());
        } else if (cmd == "show") {
            auto id = repo.rev_parse(argv[2]);
            if (!id) throw Error("bad revision");
            for (auto& f : repo.commit_changes(*id)) {
                std::printf("%s %s\n", change_name(f.change), f.path.c_str());
                std::fputs(unified_patch(f.path, repo.commit_file_diff(f)).c_str(), stdout);
            }
        } else if (cmd == "revert") {
            std::puts(repo.revert(*repo.rev_parse(argv[2])).hex().c_str());
        } else if (cmd == "undo") {
            repo.undo_last_commit();
        } else if (cmd == "rename") {
            repo.rename_branch(argv[2], argv[3]);
        } else if (cmd == "delete") {
            repo.delete_branch(argv[2]);
        } else if (cmd == "catfile") {
            auto o = repo.odb().read(*repo.rev_parse(argv[2]));
            std::fwrite(o.data.data(), 1, o.data.size(), stdout);
        } else if (cmd == "fetch") {
            fetch(repo, argc > 2 ? argv[2] : "origin", {}, {});
        } else if (cmd == "push") {
            auto br = argc > 3 ? std::string(argv[3]) : repo.current_branch();
            push(repo, argc > 2 ? argv[2] : "origin", br, {}, {}, true);
        } else if (cmd == "pull") {
            auto m = pull(repo, {}, {});
            std::printf("pull kind=%d\n", m.kind);
        } else if (cmd == "rebase") {
            auto r = repo.rebase_onto(argv[2]);
            std::printf("%s\n", r.conflicts ? "conflicts" : "done");
        } else if (cmd == "reword") {
            repo.reword(*repo.rev_parse(argv[2]), std::string(argv[3]) + "\n");
        } else if (cmd == "squash") {  // squash <into> <msg> <rev>...
            std::vector<ObjectId> cs;
            for (int i = 4; i < argc; i++) cs.push_back(*repo.rev_parse(argv[i]));
            auto into = *repo.rev_parse(argv[2]);
            cs.push_back(into);
            auto r = repo.squash_commits(cs, into, std::string(argv[3]) + "\n");
            std::printf("%s\n", r.conflicts ? "conflicts" : "done");
        } else if (cmd == "reorder") {  // reorder <before|top> <rev>...
            std::vector<ObjectId> cs;
            for (int i = 3; i < argc; i++) cs.push_back(*repo.rev_parse(argv[i]));
            ObjectId before;
            if (std::string(argv[2]) != "top") before = *repo.rev_parse(argv[2]);
            auto r = repo.reorder(cs, before);
            std::printf("%s\n", r.conflicts ? "conflicts" : "done");
        } else if (cmd == "cherry") {  // cherry <branch> <rev>...
            std::vector<ObjectId> cs;
            for (int i = 3; i < argc; i++) cs.push_back(*repo.rev_parse(argv[i]));
            auto r = repo.cherry_pick_onto(cs, argv[2]);
            std::printf("%s\n", r.conflicts ? "conflicts" : "done");
        } else if (cmd == "continue") {
            auto r = repo.rewrite_continue();
            std::printf("%s\n", r.conflicts ? "conflicts" : "done");
        } else if (cmd == "abort") {
            repo.rewrite_abort();
        } else if (cmd == "squash-merge") {
            auto m = repo.merge(argv[2], true);
            const char* k[] = {"up-to-date", "fast-forward", "merged", "conflicts"};
            std::printf("%s\n", k[m.kind]);
        } else if (cmd == "preview") {
            auto p = repo.merge_preview(argv[2]);
            const char* k[] = {"up-to-date", "fast-forward", "clean", "conflicts", "unrelated"};
            std::printf("%s commits=%d conflicts=%d\n", k[p.kind], p.commits, p.conflicts);
        } else if (cmd == "reset") {
            repo.reset_mixed(*repo.rev_parse(argv[2]));
        } else if (cmd == "tag") {
            repo.create_tag(argv[2], *repo.head());
        } else if (cmd == "deltag") {
            repo.delete_tag(argv[2]);
        } else if (cmd == "lfs-track") {
            lfs::track(repo, argv[2]);
        } else if (cmd == "lfs-pull") {
            std::printf("replaced %zu\n", lfs::pull(repo, argc > 2 ? argv[2] : "origin", {}, {}));
        } else if (cmd == "lfs-missing") {
            for (auto& m : repo.lfs_missing()) std::puts(m.c_str());
        } else if (cmd == "is-lfs") {
            std::puts(repo.is_lfs(argv[2]) ? "yes" : "no");
        } else {
            usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    return 0;
}
