// SPDX-License-Identifier: MIT
#include "transport.hpp"
#include "lfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <thread>
#include <unordered_set>

#include "../net/http.hpp"
#include "process.hpp"

namespace git {

// ================================================================ pkt-line
static std::string pkt(std::string_view s) {
    char hdr[5];
    std::snprintf(hdr, sizeof hdr, "%04x", unsigned(s.size() + 4));
    return std::string(hdr, 4) + std::string(s);
}
static const std::string FLUSH = "0000";

struct PktReader {
    std::string_view d;
    size_t pos = 0;
    // returns nullopt at end of data; empty string_view for flush
    std::optional<std::string_view> next(bool* is_flush = nullptr) {
        if (pos + 4 > d.size()) return std::nullopt;
        unsigned len = 0;
        for (int i = 0; i < 4; i++) {
            char c = d[pos + i];
            len <<= 4;
            if (c >= '0' && c <= '9') len |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') len |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') len |= unsigned(c - 'A' + 10);
            else throw Error("protocol error: bad pkt-line");
        }
        if (len < 4) {  // flush (0), delim (1), response-end (2)
            pos += 4;
            if (is_flush) *is_flush = true;
            return std::string_view{};
        }
        if (pos + len > d.size()) throw Error("protocol error: truncated pkt-line");
        auto out = d.substr(pos + 4, len - 4);
        pos += len;
        if (is_flush) *is_flush = false;
        return out;
    }
};

static std::string strip_nl(std::string_view s) {
    std::string r(s);
    if (!r.empty() && r.back() == '\n') r.pop_back();
    return r;
}

// ================================================================ urls
bool is_ssh_url(const std::string& url) {
    return starts_with(url, "ssh://") || starts_with(url, "git@") ||
           (url.find("://") == std::string::npos && url.find(':') != std::string::npos && url.find('@') != std::string::npos);
}

std::string normalize_clone_url(const std::string& in) {
    std::string s = trim(in);
    if (s.find("://") == std::string::npos && std::count(s.begin(), s.end(), '/') == 1 &&
             s.find(':') == std::string::npos && s.find(' ') == std::string::npos)
        s = "https://github.com/" + s;  // owner/repo shorthand
    return s;
}

static std::string service_base(const std::string& url) {
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

// ================================================================ HTTP + auth
struct Session {
    std::string url;
    const CredentialProvider* creds;
    std::optional<Credentials> cred{};
    int attempt = 0;
    std::unique_ptr<Process> ssh;  // live connection for ssh:// and scp-style remotes
    Session(std::string u, const CredentialProvider* c) : url(std::move(u)), creds(c) {}
    ~Session() {
        if (ssh) {
            try { ssh->write("0000"); } catch (...) {}
            ssh->close_stdin();
            ssh->wait();
        }
    }
    bool is_ssh() const { return is_ssh_url(url); }
    void ssh_spawn(const std::string& service);
    std::string rpc(const std::string& service, const std::string& body, const std::function<void(size_t)>& on_body = {});

    net::Response send(net::Request req, const std::function<void(size_t)>& on_body = {}) {
        for (;;) {
            if (cred) req.headers["Authorization"] = "Basic " + net::base64_encode(cred->username + ":" + cred->password);
            req.headers["Pragma"] = "no-cache";
            auto resp = net::request(req, on_body);
            if (resp.status == 401 || (resp.status == 403 && !cred && url.find("github.com") != std::string::npos)) {
                if (!creds || !*creds) throw Error("Authentication failed for " + url);
                auto c = (*creds)(url, attempt++);
                if (!c) throw Error("Authentication cancelled.");
                if (attempt > 4) throw Error("Authentication failed for " + url);
                cred = c;
                continue;
            }
            if (resp.status == 404) throw Error("Repository not found: " + url);
            if (resp.status >= 400)
                throw Error("The server returned HTTP " + std::to_string(resp.status) + " for " + url);
            return resp;
        }
    }
};

static void parse_refs(PktReader& pr, RemoteRefs& rr) {
    bool flush;
    bool first_ref = true;
    while (auto l = pr.next(&flush)) {
        if (flush) break;
        std::string line = strip_nl(*l);
        if (first_ref) {
            first_ref = false;
            size_t nul = line.find('\0');
            if (nul != std::string::npos) {
                std::string caps = line.substr(nul + 1);
                line = line.substr(0, nul);
                size_t p = 0;
                while (p < caps.size()) {
                    size_t sp = caps.find(' ', p);
                    std::string c = caps.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
                    if (!c.empty()) {
                        rr.caps.insert(c);
                        if (starts_with(c, "symref=HEAD:")) rr.head_symref = c.substr(12);
                    }
                    if (sp == std::string::npos) break;
                    p = sp + 1;
                }
            }
        }
        if (line.size() < 41) continue;
        auto id = ObjectId::try_hex(line.substr(0, 40));
        std::string name = line.substr(41);
        if (!id || name == "capabilities^{}") continue;
        if (name.size() > 3 && name.substr(name.size() - 3) == "^{}") {
            rr.peeled[name.substr(0, name.size() - 3)] = *id;
            continue;
        }
        rr.refs.push_back({name, *id});
    }
}

// ---------------------------------------------------------------- ssh
struct SshTarget { std::string user_host, port, path; };

static SshTarget parse_ssh_url(const std::string& url) {
    SshTarget t;
    if (starts_with(url, "ssh://")) {
        std::string rest = url.substr(6);
        size_t slash = rest.find('/');
        std::string auth = rest.substr(0, slash);
        t.path = slash == std::string::npos ? "/" : rest.substr(slash);
        size_t colon = auth.rfind(':');
        if (colon != std::string::npos && auth.find(']') == std::string::npos) {
            t.port = auth.substr(colon + 1);
            auth = auth.substr(0, colon);
        }
        t.user_host = auth;
        // ssh://host/~user/repo keeps the ~
        if (starts_with(t.path, "/~")) t.path = t.path.substr(1);
    } else {
        size_t colon = url.find(':');
        t.user_host = url.substr(0, colon);
        t.path = url.substr(colon + 1);
    }
    return t;
}

static std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    char q = 0;
    for (char c : s) {
        if (q) { if (c == q) q = 0; else cur += c; continue; }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == ' ' || c == '\t') { if (!cur.empty()) out.push_back(cur), cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void Session::ssh_spawn(const std::string& service) {
    auto t = parse_ssh_url(url);
    std::vector<std::string> argv;
    if (auto cmd = std::getenv("GIT_SSH_COMMAND"); cmd && *cmd) argv = split_words(cmd);
    else if (auto prog = std::getenv("GIT_SSH"); prog && *prog) argv = {prog};
    else argv = {"ssh"};
    if (find_program(argv[0]).empty()) throw Error("SSH remotes need the `ssh` program, which wasn't found on PATH.");
    // No terminal to answer prompts: accept new host keys once (TOFU) and fail fast on passwords.
    argv.insert(argv.end(), {"-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new"});
    if (!t.port.empty()) argv.insert(argv.end(), {"-p", t.port});
    argv.push_back(t.user_host);
    std::string quoted = "'";
    for (char c : t.path) { if (c == '\'') quoted += "'\\''"; else quoted += c; }
    quoted += "'";
    argv.push_back(service + " " + quoted);
    ssh = Process::spawn(argv, fs::current_path(), {}, false);
}

static std::string read_until_flush(Process& p) {
    // read pkt-lines until the first flush packet
    std::string buf;
    auto read_exact = [&](size_t n) {
        size_t start = buf.size();
        buf.resize(start + n);
        size_t got = 0;
        while (got < n) {
            size_t r = p.read(buf.data() + start + got, n - got);
            if (r == 0) {
                std::string err = p.read_stderr_all();
                throw Error("SSH connection closed: " + trim(err.empty() ? "no response from server" : err));
            }
            got += r;
        }
    };
    for (;;) {
        read_exact(4);
        unsigned len = unsigned(std::stoul(buf.substr(buf.size() - 4), nullptr, 16));
        if (len == 0) return buf;
        read_exact(len - 4);
    }
}

std::string Session::rpc(const std::string& service, const std::string& body, const std::function<void(size_t)>& on_body) {
    if (is_ssh()) {
        if (!ssh) throw Error("protocol error: no SSH connection");
        std::string out;
        std::thread writer([&] {
            try { ssh->write(body); } catch (...) {}
            ssh->close_stdin();
        });
        char tmp[65536];
        size_t n;
        while ((n = ssh->read(tmp, sizeof tmp)) > 0) {
            out.append(tmp, n);
            if (on_body) on_body(out.size());
        }
        writer.join();
        std::string err = ssh->read_stderr_all();
        int code = ssh->wait();
        ssh.reset();
        if (code != 0 && out.empty()) throw Error("SSH " + service + " failed: " + trim(err));
        return out;
    }
    net::Request req;
    req.method = "POST";
    req.url = service_base(url) + "/" + service;
    req.headers["Content-Type"] = "application/x-" + service + "-request";
    req.headers["Accept"] = "application/x-" + service + "-result";
    req.body = body;
    return send(req, on_body).body;
}

static RemoteRefs discover(Session& s, const std::string& service) {
    RemoteRefs rr;
    if (s.is_ssh()) {
        s.ssh_spawn(service);
        std::string adv = read_until_flush(*s.ssh);
        PktReader pr{adv};
        parse_refs(pr, rr);
        return rr;
    }
    net::Request req;
    req.url = service_base(s.url) + "/info/refs?service=" + service;
    auto resp = s.send(req);
    PktReader pr{resp.body};
    bool smart = starts_with(resp.header("content-type"), "application/x-" + service + "-advertisement");
    if (!smart) throw Error("The server at " + s.url + " doesn't speak the smart HTTP git protocol.");
    bool flush;
    auto first = pr.next(&flush);
    if (!first || !starts_with(*first, "# service=")) throw Error("protocol error: missing service header");
    while (auto l = pr.next(&flush)) if (flush) break;
    parse_refs(pr, rr);
    return rr;
}

RemoteRefs ls_remote(const std::string& url, const std::string& service, const CredentialProvider& creds) {
    Session s{url, &creds};
    return discover(s, service);
}

// Parse side-band-64k multiplexed stream: band 1 is pack data.
static std::string demux(std::string_view body, size_t start, const ProgressFn& progress, std::string* text_out = nullptr) {
    PktReader pr{body, start};
    std::string pack;
    bool flush;
    while (auto l = pr.next(&flush)) {
        if (flush) continue;
        if (l->empty()) continue;
        char band = (*l)[0];
        auto payload = l->substr(1);
        if (band == 1) pack.append(payload);
        else if (band == 2) {
            if (progress) {
                Progress p;
                std::string msg(payload);
                // keep only the last carriage-return separated message
                auto cut = msg.find_last_of("\r\n", msg.size() > 1 ? msg.size() - 2 : 0);
                if (cut != std::string::npos && cut + 1 < msg.size()) msg = msg.substr(cut + 1);
                p.phase = "Remote";
                p.remote_message = trim(msg);
                progress(p);
            }
        } else if (band == 3) {
            throw Error("remote: " + trim(std::string(payload)));
        } else {
            // not side-band (shouldn't happen when negotiated)
            pack.append(*l);
        }
    }
    if (text_out) *text_out = pack;
    return pack;
}

// ================================================================ fetch
static std::vector<ObjectId> collect_haves(Repository& repo, size_t limit) {
    std::vector<ObjectId> out;
    std::unordered_set<ObjectId, ObjectIdHash> seen;
    std::deque<ObjectId> q;
    for (auto& t : repo.local_commit_tips()) q.push_back(t);
    while (!q.empty() && out.size() < limit) {
        auto id = q.front();
        q.pop_front();
        if (!seen.insert(id).second) continue;
        auto o = repo.odb().try_read(id);
        if (!o) continue;
        if (o->type == ObjType::Tag) { q.push_back(Tag::parse(o->data).object); continue; }
        if (o->type != ObjType::Commit) continue;
        out.push_back(id);
        for (auto& p : Commit::parse(o->data).parents) q.push_back(p);
    }
    return out;
}

static void receive_pack_into(Repository& repo, std::string_view pack, const ProgressFn& progress) {
    if (pack.empty()) return;
    auto pack_dir = repo.odb().dir() / "pack";
    index_pack(pack, pack_dir,
               [&](const ObjectId& id) { return repo.odb().try_read(id); },
               [&](size_t done, size_t total) {
                   if (progress) progress({"Resolving deltas", done, total, ""});
               });
    repo.odb().reload_packs();
}

static void fetch_objects(Repository& repo, Session& s, const std::string& url, const RemoteRefs& rr,
                          const std::vector<ObjectId>& wants, const ProgressFn& progress) {
    if (wants.empty()) return;
    std::vector<std::string> use;
    for (auto c : {"side-band-64k", "ofs-delta"}) if (rr.caps.count(c)) use.push_back(c);
    std::string capstr;
    for (auto& c : use) capstr += " " + c;
    capstr += " agent=git/2.47.0-tributary";
    std::string body;
    for (size_t i = 0; i < wants.size(); i++)
        body += pkt("want " + wants[i].hex() + (i == 0 ? capstr : "") + "\n");
    body += FLUSH;
    for (auto& h : collect_haves(repo, 512)) body += pkt("have " + h.hex() + "\n");
    body += pkt("done\n");
    (void)url;
    std::string resp_body = s.rpc("git-upload-pack", body, [&](size_t n) {
        if (progress) progress({"Receiving objects", n, 0, ""});
    });
    PktReader pr{resp_body};
    size_t pack_start = 0;
    for (;;) {
        size_t before = pr.pos;
        bool flush;
        auto l = pr.next(&flush);
        if (!l) break;
        if (flush) continue;
        std::string t = strip_nl(*l);
        if (t == "NAK" || starts_with(t, "ACK ")) { pack_start = pr.pos; continue; }
        pack_start = before;
        break;
    }
    std::string pack = rr.caps.count("side-band-64k") ? demux(resp_body, pack_start, progress) : resp_body.substr(pack_start);
    receive_pack_into(repo, pack, progress);
}

FetchResult fetch(Repository& repo, const std::string& remote, const CredentialProvider& creds,
                  const ProgressFn& progress) {
    auto url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    Session s{*url, &creds};
    if (progress) progress({"Connecting", 0, 0, ""});
    auto rr = discover(s, "git-upload-pack");
    FetchResult res;
    if (starts_with(rr.head_symref, "refs/heads/")) res.default_branch = rr.head_symref.substr(11);

    // which refs do we want
    std::vector<std::pair<std::string, ObjectId>> heads, tags;
    for (auto& [name, id] : rr.refs) {
        if (starts_with(name, "refs/heads/")) heads.push_back({name, id});
        else if (starts_with(name, "refs/tags/")) tags.push_back({name, id});
    }
    std::vector<ObjectId> wants;
    std::unordered_set<ObjectId, ObjectIdHash> wanted;
    auto want = [&](const ObjectId& id) {
        if (!repo.odb().exists(id) && wanted.insert(id).second) wants.push_back(id);
    };
    for (auto& [n, id] : heads) want(id);
    for (auto& [n, id] : tags) want(id);

    fetch_objects(repo, s, *url, rr, wants, progress);

    // update remote-tracking refs
    auto who = Signature::now(repo.user_name().value_or("Tributary"), repo.user_email().value_or(""));
    std::set<std::string> remote_names;
    for (auto& [name, id] : heads) {
        std::string local = "refs/remotes/" + remote + "/" + name.substr(11);
        remote_names.insert(local);
        auto cur = repo.refs().resolve(local);
        if (cur && *cur == id) continue;
        repo.refs().update(local, id, cur ? "fetch: fast-forward" : "fetch: storing head", who);
        res.updated.push_back(local);
    }
    // prune deleted remote branches
    for (auto& r : repo.refs().list("refs/remotes/" + remote + "/")) {
        if (r.name == "refs/remotes/" + remote + "/HEAD") continue;
        if (!remote_names.count(r.name)) repo.refs().remove(r.name);
    }
    for (auto& [name, id] : tags) {
        if (repo.refs().resolve(name)) continue;
        if (!repo.odb().exists(id)) continue;
        repo.refs().update(name, id, "fetch: tag", who);
    }
    if (!res.default_branch.empty())
        repo.refs().set_symbolic("refs/remotes/" + remote + "/HEAD", "refs/remotes/" + remote + "/" + res.default_branch);
    write_file(repo.gitdir() / "FETCH_HEAD", "");
    return res;
}

// ================================================================ push
static void send_receive_pack(Repository& repo, Session& s, const std::string& url, RemoteRefs& rr,
                              const std::vector<std::tuple<ObjectId, ObjectId, std::string>>& updates,
                              const std::vector<ObjectId>& new_tips, const ProgressFn& progress) {
    std::string capstr;
    if (rr.caps.count("report-status")) capstr += " report-status";
    if (rr.caps.count("side-band-64k")) capstr += " side-band-64k";
    if (rr.caps.count("delete-refs")) capstr += " delete-refs";
    capstr += " agent=git/2.47.0-tributary";
    std::string body;
    for (size_t i = 0; i < updates.size(); i++) {
        auto& [o, n, ref] = updates[i];
        std::string line = o.hex() + " " + n.hex() + " " + ref;
        if (i == 0) line += '\0' + capstr.substr(1);
        body += pkt(line + "\n");
    }
    body += FLUSH;
    bool any_create = false;
    for (auto& u : updates) if (!std::get<1>(u).is_zero()) any_create = true;
    if (any_create) {
        std::set<ObjectId> stop;
        for (auto& [name, id] : rr.refs) if (repo.odb().exists(id)) stop.insert(id);
        if (progress) progress({"Counting objects", 0, 0, ""});
        auto ids = repo.reachable_objects(new_tips, stop);
        std::vector<std::pair<ObjectId, Object>> objs;
        size_t i = 0;
        for (auto& id : ids) {
            objs.push_back({id, repo.odb().read(id)});
            if (progress && (++i % 64 == 0)) progress({"Compressing objects", i, ids.size(), ""});
        }
        if (progress) progress({"Writing objects", 0, objs.size(), ""});
        body += build_pack(objs);
    }
    (void)url;
    std::string resp_body = s.rpc("git-receive-pack", body);
    // parse report
    std::string report = rr.caps.count("side-band-64k") ? demux(resp_body, 0, progress) : resp_body;
    PktReader pr{report};
    std::string errors;
    bool flush;
    while (auto l = pr.next(&flush)) {
        if (flush) continue;
        std::string t = strip_nl(*l);
        if (starts_with(t, "unpack ") && t != "unpack ok") errors += "unpack failed: " + t.substr(7) + "\n";
        if (starts_with(t, "ng ")) errors += "rejected " + t.substr(3) + "\n";
    }
    if (!errors.empty()) {
        if (errors.find("non-fast-forward") != std::string::npos || errors.find("fetch first") != std::string::npos)
            throw Error("The remote has commits you don't have yet. Pull first, then push.");
        throw Error(trim(errors));
    }
}

void push(Repository& repo, const std::string& remote, const std::string& branch, const CredentialProvider& creds,
          const ProgressFn& progress, bool set_upstream, bool force) {
    auto url = repo.config().repo.get("remote." + remote + ".pushurl");
    if (!url) url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    auto local = repo.refs().resolve("refs/heads/" + branch);
    if (!local) throw Error("no local branch " + branch);
    // the remote branch name follows the upstream config if present
    std::string remote_ref = "refs/heads/" + branch;
    if (auto m = repo.config().repo.get("branch." + branch + ".merge")) remote_ref = *m;
    Session s{*url, &creds};
    if (progress) progress({"Connecting", 0, 0, ""});
    auto rr = discover(s, "git-receive-pack");
    ObjectId old;
    for (auto& [n, id] : rr.refs) if (n == remote_ref) old = id;
    if (old == *local) {
        if (set_upstream) repo.set_upstream(branch, remote, remote_ref);
        return;
    }
    if (!old.is_zero() && !force) {
        if (!repo.odb().exists(old) || !repo.is_ancestor(old, *local))
            throw Error("The remote has commits you don't have yet. Pull first, then push.");
    }
    // pre-push hook: "<local ref> <local sha> <remote ref> <remote sha>"
    if (auto r = run_hook(repo.hooks_dir(), repo.gitdir(), repo.worktree(), "pre-push", {remote, *url},
                          "refs/heads/" + branch + " " + local->hex() + " " + remote_ref + " " + old.hex() + "\n");
        r && r->exit_code != 0)
        throw Error("The pre-push hook failed:\n\n" + trim(r->output));
    // Git LFS: upload the objects behind any pointers we're about to push (like git-lfs's pre-push)
    {
        std::set<ObjectId> stop;
        for (auto& [name, id] : rr.refs) if (repo.odb().exists(id)) stop.insert(id);
        auto ptrs = lfs::pointers_in(repo, repo.reachable_objects({*local}, stop));
        if (!ptrs.empty()) lfs::upload(repo, remote, ptrs, creds, progress, remote_ref);
    }
    send_receive_pack(repo, s, *url, rr, {{old, *local, remote_ref}}, {*local}, progress);
    auto who = Signature::now(repo.user_name().value_or("Tributary"), repo.user_email().value_or(""));
    std::string tracking = "refs/remotes/" + remote + "/" + remote_ref.substr(11);
    repo.refs().update(tracking, *local, "update by push", who);
    if (set_upstream) repo.set_upstream(branch, remote, remote_ref);
}

void push_delete(Repository& repo, const std::string& remote, const std::string& branch,
                 const CredentialProvider& creds) {
    auto url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    Session s{*url, &creds};
    auto rr = discover(s, "git-receive-pack");
    std::string ref = "refs/heads/" + branch;
    ObjectId old;
    for (auto& [n, id] : rr.refs) if (n == ref) old = id;
    if (old.is_zero()) return;
    send_receive_pack(repo, s, *url, rr, {{old, ObjectId{}, ref}}, {}, {});
    repo.refs().remove("refs/remotes/" + remote + "/" + branch);
}

void push_delete_ref(Repository& repo, const std::string& remote, const std::string& ref, const CredentialProvider& creds) {
    auto url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    Session s{*url, &creds};
    auto rr = discover(s, "git-receive-pack");
    ObjectId old;
    for (auto& [n, id] : rr.refs) if (n == ref) old = id;
    if (old.is_zero()) return;
    send_receive_pack(repo, s, *url, rr, {{old, ObjectId{}, ref}}, {}, {});
}

void push_tags(Repository& repo, const std::string& remote, const CredentialProvider& creds) {
    auto url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    Session s{*url, &creds};
    auto rr = discover(s, "git-receive-pack");
    std::set<std::string> have;
    for (auto& [n, _] : rr.refs) have.insert(n);
    std::vector<std::tuple<ObjectId, ObjectId, std::string>> ups;
    std::vector<ObjectId> tips;
    for (auto& [name, id] : repo.tags()) {
        if (have.count("refs/tags/" + name)) continue;
        ups.push_back({ObjectId{}, id, "refs/tags/" + name});
        tips.push_back(id);
    }
    if (!ups.empty()) send_receive_pack(repo, s, *url, rr, ups, tips, {});
}

// ================================================================ clone / pull
Repository clone(const std::string& url_in, const fs::path& dir, const CredentialProvider& creds,
                 const ProgressFn& progress) {
    std::string url = normalize_clone_url(url_in);
    std::error_code ec;
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) throw Error("The destination " + path_to_git(dir) + " already exists and isn't empty.");
    bool created = !fs::exists(dir, ec);
    try {
        auto repo = Repository::init(dir, "main");
        repo.add_remote("origin", url);
        auto fr = fetch(repo, "origin", creds, progress);
        std::string def = fr.default_branch;
        if (def.empty()) {
            for (auto c : {"main", "master"})
                if (repo.refs().resolve(std::string("refs/remotes/origin/") + c)) { def = c; break; }
            if (def.empty()) {
                auto rem = repo.refs().list("refs/remotes/origin/");
                for (auto& r : rem) if (r.name != "refs/remotes/origin/HEAD") { def = r.name.substr(20); break; }
            }
        }
        if (def.empty()) {
            // empty repository
            return repo;
        }
        auto tip = *repo.refs().resolve("refs/remotes/origin/" + def);
        auto who = Signature::now(repo.user_name().value_or("Tributary"), repo.user_email().value_or(""));
        if (progress) progress({"Checking out files", 0, 0, ""});
        repo.switch_tree(std::nullopt, repo.odb().read_commit(tip).tree, true);
        repo.refs().update("refs/heads/" + def, tip, "clone: from " + url, who);
        repo.refs().set_symbolic("HEAD", "refs/heads/" + def);
        repo.set_upstream(def, "origin", "refs/heads/" + def);
        // Git LFS files come down after checkout; if that fails the clone still stands
        // with pointer files, and `lfs pull` (Repository → Download LFS files) retries.
        if (!repo.lfs_missing().empty()) {
            try { lfs::pull(repo, "origin", creds, progress); } catch (...) {}
        }
        return repo;
    } catch (...) {
        if (created) fs::remove_all(dir, ec);
        else fs::remove_all(dir / ".git", ec);
        throw;
    }
}

void fetch_refs(Repository& repo, const std::string& remote,
                const std::vector<std::pair<std::string, std::string>>& src_dst, const CredentialProvider& creds,
                const ProgressFn& progress) {
    auto url = repo.config().repo.get("remote." + remote + ".url");
    if (!url) throw Error("no remote named " + remote);
    Session s{*url, &creds};
    auto rr = discover(s, "git-upload-pack");
    std::vector<ObjectId> wants;
    std::vector<std::pair<std::string, ObjectId>> updates;
    for (auto& [src, dst] : src_dst) {
        bool found = false;
        for (auto& [name, id] : rr.refs)
            if (name == src) {
                found = true;
                if (!repo.odb().exists(id)) wants.push_back(id);
                updates.push_back({dst, id});
            }
        if (!found) throw Error("The remote doesn't have " + src);
    }
    fetch_objects(repo, s, *url, rr, wants, progress);
    auto who = Signature::now(repo.user_name().value_or("Tributary"), repo.user_email().value_or(""));
    for (auto& [dst, id] : updates) repo.refs().update(dst, id, "fetch: " + dst, who);
}

MergeOutcome pull(Repository& repo, const CredentialProvider& creds, const ProgressFn& progress) {
    auto branch = repo.current_branch();
    if (branch.empty()) throw Error("You're not on a branch (detached HEAD).");
    auto remote = repo.config().repo.get("branch." + branch + ".remote").value_or("origin");
    fetch(repo, remote, creds, progress);
    auto up = repo.upstream_of(branch);
    if (up.empty()) {
        up = "refs/remotes/" + remote + "/" + branch;
        if (!repo.refs().resolve(up)) throw Error("The current branch has no upstream branch on " + remote + ".");
    }
    if (!repo.refs().resolve(up)) throw Error("The upstream branch " + shorten_ref(up) + " no longer exists.");
    auto out = repo.merge(up);
    if (!repo.lfs_missing().empty()) lfs::pull(repo, remote, creds, progress);
    return out;
}

}  // namespace git
