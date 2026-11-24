// SPDX-License-Identifier: MIT
#include "lfs.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "process.hpp"
#include "../net/http.hpp"
#include "../net/json.hpp"

namespace git {

// ================================================================ attributes
const std::vector<Repository::AttrRule>& Repository::attr_rules(const std::string& dir) const {
    auto it = attr_cache_.find(dir);
    if (it != attr_cache_.end()) return it->second;
    std::vector<AttrRule> rules;
    fs::path p = dir == "\x01info" ? gitdir_ / "info" / "attributes" : (dir.empty() ? wt_ / ".gitattributes" : wt_ / git_to_path(dir) / ".gitattributes");
    if (auto d = try_read_file(p)) {
        size_t pos = 0;
        while (pos < d->size()) {
            size_t nl = d->find('\n', pos);
            std::string line = d->substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? d->size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> words;
            std::string cur;
            for (char c : line) {
                if (c == ' ' || c == '\t') { if (!cur.empty()) words.push_back(cur), cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) words.push_back(cur);
            if (words.size() < 2) continue;
            std::optional<bool> lfs;
            for (size_t i = 1; i < words.size(); i++) {
                auto& w = words[i];
                if (w == "filter=lfs") lfs = true;
                else if (starts_with(w, "filter=") || w == "-filter" || w == "!filter") lfs = false;
            }
            if (lfs) rules.push_back({words[0], *lfs});
        }
    }
    return attr_cache_.emplace(dir, std::move(rules)).first->second;
}

bool Repository::is_lfs(const std::string& path) const {
    bool result = false;
    auto apply = [&](const std::string& dir, const std::vector<AttrRule>& rules) {
        std::string rel = dir.empty() || dir == "\x01info" ? path : path.substr(dir.size() + 1);
        std::string base = rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1);
        for (auto& r : rules) {
            std::string pat = r.pattern;
            if (!pat.empty() && pat.back() == '/') continue;  // directories don't carry attributes
            bool anchored = pat.find('/') != std::string::npos;
            if (!pat.empty() && pat[0] == '/') pat = pat.substr(1);
            if (anchored ? wildmatch(pat, rel, true) : wildmatch(pat, base, false)) result = r.lfs;
        }
    };
    // lowest priority first: root, then deeper directories, then .git/info/attributes
    apply("", attr_rules(""));
    for (size_t sl = path.find('/'); sl != std::string::npos; sl = path.find('/', sl + 1)) {
        std::string d = path.substr(0, sl);
        auto& rules = attr_rules(d);
        if (!rules.empty()) apply(d, rules);
    }
    apply("\x01info", attr_rules("\x01info"));
    return result;
}

fs::path Repository::lfs_object_path(const std::string& oid) const {
    return gitdir_ / "lfs" / "objects" / oid.substr(0, 2) / oid.substr(2, 2) / oid;
}

Bytes Repository::to_odb(const std::string& path, const Bytes& raw, bool store) const {
    if (!is_lfs(path)) return to_odb_eol(path, raw);
    if (raw.size() < 1024 && lfs::parse_pointer(raw)) return raw;  // already a pointer (not smudged)
    lfs::Pointer p{Sha256::hex_of(raw), raw.size()};
    if (store) {
        auto op = lfs_object_path(p.oid);
        std::error_code ec;
        if (!fs::exists(op, ec)) {
            fs::create_directories(op.parent_path(), ec);
            write_file_atomic(op, raw);
        }
    }
    return lfs::make_pointer(p);
}

Bytes Repository::smudged(const std::string&, const Bytes& blob) const {
    if (blob.size() >= 1024) return blob;
    auto p = lfs::parse_pointer(blob);
    if (!p) return blob;
    if (auto d = try_read_file(lfs_object_path(p->oid))) return *d;
    return blob;
}

namespace lfs {

static const char* kVersion = "version https://git-lfs.github.com/spec/v1";

std::optional<Pointer> parse_pointer(std::string_view d) {
    if (d.size() > 1024) return std::nullopt;
    if (!(starts_with(d, kVersion) || starts_with(d, "version https://hawser.github.com/spec/v1"))) return std::nullopt;
    Pointer p;
    bool have_oid = false, have_size = false;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t nl = d.find('\n', pos);
        std::string_view line = d.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? d.size() : nl + 1;
        if (starts_with(line, "oid sha256:")) {
            p.oid = std::string(line.substr(11));
            have_oid = p.oid.size() == 64 && std::all_of(p.oid.begin(), p.oid.end(), [](char c) { return std::isxdigit((unsigned char)c); });
        } else if (starts_with(line, "size ")) {
            try { p.size = std::stoull(std::string(line.substr(5))); have_size = true; } catch (...) {}
        }
    }
    if (!have_oid || !have_size) return std::nullopt;
    return p;
}

std::string make_pointer(const Pointer& p) {
    return std::string(kVersion) + "\noid sha256:" + p.oid + "\nsize " + std::to_string(p.size) + "\n";
}

// ---------------------------------------------------------------- endpoint
struct Endpoint {
    std::string href;
    std::map<std::string, std::string> headers;
};

static std::string https_endpoint_for(const std::string& remote_url) {
    std::string u = remote_url;
    // scp-like / ssh:// remotes map to https on the same host
    if (is_ssh_url(u)) {
        std::string host, path;
        if (starts_with(u, "ssh://")) {
            std::string rest = u.substr(6);
            size_t sl = rest.find('/');
            host = rest.substr(0, sl);
            path = sl == std::string::npos ? "" : rest.substr(sl + 1);
        } else {
            size_t colon = u.find(':');
            host = u.substr(0, colon);
            path = u.substr(colon + 1);
        }
        if (auto at = host.find('@'); at != std::string::npos) host = host.substr(at + 1);
        if (auto c = host.find(':'); c != std::string::npos) host = host.substr(0, c);
        u = "https://" + host + "/" + path;
    }
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() < 4 || u.substr(u.size() - 4) != ".git") u += ".git";
    return u + "/info/lfs";
}

static Endpoint endpoint(Repository& repo, const std::string& remote, const std::string& operation) {
    auto& cfg = repo.config();
    if (auto u = cfg.get("lfs.url")) return {*u, {}};
    if (auto u = cfg.get("remote." + remote + ".lfsurl")) return {*u, {}};
    auto url = cfg.get("remote." + remote + ".url");
    if (!url) throw Error("There's no remote named " + remote + ".");
    if (is_ssh_url(*url)) {
        // git-lfs-authenticate hands out an href and a short-lived Authorization header
        std::string user_host, path, port;
        if (starts_with(*url, "ssh://")) {
            std::string rest = url->substr(6);
            size_t sl = rest.find('/');
            user_host = rest.substr(0, sl);
            path = sl == std::string::npos ? "" : rest.substr(sl);
            if (auto c = user_host.rfind(':'); c != std::string::npos) { port = user_host.substr(c + 1); user_host = user_host.substr(0, c); }
        } else {
            size_t colon = url->find(':');
            user_host = url->substr(0, colon);
            path = url->substr(colon + 1);
        }
        std::vector<std::string> argv;
        if (auto cmd = std::getenv("GIT_SSH_COMMAND"); cmd && *cmd) {
            std::string cur;
            for (char c : std::string(cmd)) { if (c == ' ') { if (!cur.empty()) argv.push_back(cur), cur.clear(); } else cur += c; }
            if (!cur.empty()) argv.push_back(cur);
        } else if (auto prog = std::getenv("GIT_SSH"); prog && *prog) argv = {prog};
        else argv = {"ssh"};
        argv.insert(argv.end(), {"-o", "BatchMode=yes"});
        if (!port.empty()) argv.insert(argv.end(), {"-p", port});
        argv.push_back(user_host);
        argv.push_back("git-lfs-authenticate " + path + " " + operation);
        auto r = run_process(argv, repo.worktree());
        if (r.exit_code == 0) {
            try {
                auto v = json::parse(r.output.substr(r.output.find('{')));
                Endpoint e;
                e.href = v["href"].str();
                if (v["header"].is_object())
                    for (auto& [k, hv] : v["header"].obj()) e.headers[k] = hv.str();
                if (!e.href.empty()) return e;
            } catch (...) {}
        }
    }
    return {https_endpoint_for(*url), {}};
}

// ---------------------------------------------------------------- HTTP with git's credentials
struct Client {
    const CredentialProvider* creds;
    std::optional<Credentials> cred;
    int attempt = 0;
    net::Response send(net::Request req, bool auth_ok_for_host = true, const std::function<void(size_t)>& on_body = {}) {
        for (;;) {
            if (cred && auth_ok_for_host && !req.headers.count("Authorization"))
                req.headers["Authorization"] = "Basic " + net::base64_encode(cred->username + ":" + cred->password);
            auto resp = net::request(req, on_body);
            if (resp.status == 401 && auth_ok_for_host && creds && *creds && !req.headers.count("X-No-Retry")) {
                auto c = (*creds)(req.url, attempt++);
                if (!c || attempt > 4) throw Error("Git LFS: authentication failed for " + req.url);
                cred = c;
                req.headers.erase("Authorization");
                continue;
            }
            return resp;
        }
    }
};

static json::Value batch(Client& cl, const Endpoint& ep, const std::string& operation, const std::vector<Pointer>& objs,
                         const std::string& ref) {
    json::Array arr;
    for (auto& o : objs) arr.push_back(json::Object{{"oid", o.oid}, {"size", int64_t(o.size)}});
    json::Object body{{"operation", operation}, {"transfers", json::Array{"basic"}}, {"objects", arr}, {"hash_algo", "sha256"}};
    if (!ref.empty()) body["ref"] = json::Object{{"name", ref}};
    net::Request rq;
    rq.method = "POST";
    rq.url = ep.href + "/objects/batch";
    rq.headers = ep.headers;
    rq.headers["Accept"] = "application/vnd.git-lfs+json";
    rq.headers["Content-Type"] = "application/vnd.git-lfs+json";
    rq.body = json::Value(body).dump();
    auto r = cl.send(rq);
    json::Value v;
    try { v = json::parse(r.body); } catch (...) {}
    if (r.status >= 400) {
        std::string msg = v["message"].str("HTTP " + std::to_string(r.status));
        throw Error("Git LFS: the server refused the " + operation + " (" + msg + ").");
    }
    return v;
}

static std::map<std::string, std::string> action_headers(const json::Value& act) {
    std::map<std::string, std::string> h;
    if (act["header"].is_object())
        for (auto& [k, v] : act["header"].obj()) h[k] = v.str();
    return h;
}

void download(Repository& repo, const std::string& remote, const std::vector<Pointer>& objects, const CredentialProvider& creds,
              const ProgressFn& progress) {
    std::vector<Pointer> need;
    std::set<std::string> seen;
    std::error_code ec;
    for (auto& o : objects)
        if (seen.insert(o.oid).second && !fs::exists(repo.lfs_object_path(o.oid), ec)) need.push_back(o);
    if (need.empty()) return;
    auto ep = endpoint(repo, remote, "download");
    Client cl{&creds};
    size_t done = 0;
    for (size_t start = 0; start < need.size(); start += 100) {
        std::vector<Pointer> chunk(need.begin() + long(start), need.begin() + long(std::min(need.size(), start + 100)));
        auto v = batch(cl, ep, "download", chunk, "");
        for (auto& o : v["objects"].arr()) {
            std::string oid = o["oid"].str();
            if (o["error"].is_object())
                throw Error("Git LFS: couldn't download " + oid.substr(0, 12) + "… (" + o["error"]["message"].str("error") + ").");
            auto& act = o["actions"]["download"];
            if (!act.is_object()) continue;
            if (progress) progress({"Downloading LFS objects", done, need.size(), ""});
            net::Request rq;
            rq.url = act["href"].str();
            rq.headers = action_headers(act);
            // only send our credentials to the LFS host itself, not to a CDN
            bool same_host = net::Url::parse(rq.url).host == net::Url::parse(ep.href).host;
            auto r = cl.send(rq, same_host && !rq.headers.count("Authorization"));
            if (r.status >= 400) throw Error("Git LFS: downloading " + oid.substr(0, 12) + "… failed with HTTP " + std::to_string(r.status) + ".");
            if (Sha256::hex_of(r.body) != oid) throw Error("Git LFS: object " + oid.substr(0, 12) + "… arrived corrupted.");
            auto path = repo.lfs_object_path(oid);
            fs::create_directories(path.parent_path(), ec);
            write_file_atomic(path, r.body);
            done++;
        }
    }
    if (progress) progress({"Downloading LFS objects", done, need.size(), ""});
}

void upload(Repository& repo, const std::string& remote, const std::vector<Pointer>& objects, const CredentialProvider& creds,
            const ProgressFn& progress, const std::string& ref) {
    if (objects.empty()) return;
    std::vector<Pointer> objs;
    std::set<std::string> seen;
    std::error_code ec;
    for (auto& o : objects) {
        if (!seen.insert(o.oid).second) continue;
        if (!fs::exists(repo.lfs_object_path(o.oid), ec))
            throw Error("Git LFS: the object for a file you're pushing (" + o.oid.substr(0, 12) + "…) is missing locally.");
        objs.push_back(o);
    }
    auto ep = endpoint(repo, remote, "upload");
    Client cl{&creds};
    size_t done = 0;
    for (size_t start = 0; start < objs.size(); start += 100) {
        std::vector<Pointer> chunk(objs.begin() + long(start), objs.begin() + long(std::min(objs.size(), start + 100)));
        auto v = batch(cl, ep, "upload", chunk, ref);
        for (auto& o : v["objects"].arr()) {
            std::string oid = o["oid"].str();
            if (o["error"].is_object())
                throw Error("Git LFS: the server won't take " + oid.substr(0, 12) + "… (" + o["error"]["message"].str("error") + ").");
            auto& up = o["actions"]["upload"];
            if (!up.is_object()) { done++; continue; }  // the server already has it
            if (progress) progress({"Uploading LFS objects", done, objs.size(), ""});
            net::Request rq;
            rq.method = "PUT";
            rq.url = up["href"].str();
            rq.headers = action_headers(up);
            rq.headers["Content-Type"] = "application/octet-stream";
            rq.body = read_file(repo.lfs_object_path(oid));
            bool same_host = net::Url::parse(rq.url).host == net::Url::parse(ep.href).host;
            auto r = cl.send(rq, same_host && !rq.headers.count("Authorization"));
            if (r.status >= 400) throw Error("Git LFS: uploading " + oid.substr(0, 12) + "… failed with HTTP " + std::to_string(r.status) + ".");
            auto& ver = o["actions"]["verify"];
            if (ver.is_object()) {
                net::Request vr;
                vr.method = "POST";
                vr.url = ver["href"].str();
                vr.headers = action_headers(ver);
                vr.headers["Content-Type"] = "application/vnd.git-lfs+json";
                vr.headers["Accept"] = "application/vnd.git-lfs+json";
                vr.body = json::Value(json::Object{{"oid", oid}, {"size", o["size"].integer()}}).dump();
                auto r2 = cl.send(vr);
                if (r2.status >= 400) throw Error("Git LFS: the server couldn't verify " + oid.substr(0, 12) + "….");
            }
            done++;
        }
    }
    if (progress) progress({"Uploading LFS objects", done, objs.size(), ""});
}

std::vector<Pointer> pointers_in(Repository& repo, const std::set<ObjectId>& objects) {
    std::vector<Pointer> out;
    for (auto& id : objects) {
        auto o = repo.odb().try_read(id);
        if (!o || o->type != ObjType::Blob || o->data.size() > 1024) continue;
        if (auto p = parse_pointer(o->data)) out.push_back(*p);
    }
    return out;
}

size_t pull(Repository& repo, const std::string& remote, const CredentialProvider& creds, const ProgressFn& progress) {
    auto h = repo.head();
    if (!h) return 0;
    std::map<std::string, FlatEntry> files;
    flatten_tree(repo.odb(), repo.odb().read_commit(*h).tree, "", files);
    std::vector<std::pair<std::string, Pointer>> want;
    std::vector<Pointer> objs;
    for (auto& [path, fe] : files) {
        if (fe.mode == 0160000 || fe.mode == 0120000) continue;
        auto o = repo.odb().try_read(fe.id);
        if (!o || o->data.size() > 1024) continue;
        auto p = parse_pointer(o->data);
        if (!p) continue;
        want.push_back({path, *p});
        objs.push_back(*p);
    }
    if (want.empty()) return 0;
    download(repo, remote, objs, creds, progress);
    size_t replaced = 0;
    for (auto& [path, p] : want) {
        auto wp = repo.worktree() / git_to_path(path);
        auto cur = try_read_file(wp);
        // only replace files that are still the pointer (never clobber edits)
        if (!cur || cur->size() > 1024) continue;
        auto cp = parse_pointer(*cur);
        if (!cp || cp->oid != p.oid) continue;
        repo.write_worktree_file(path, make_pointer(p), files[path].mode);
        replaced++;
    }
    repo.clear_lfs_missing();
    return replaced;
}

void track(Repository& repo, const std::string& pattern) {
    auto p = repo.worktree() / ".gitattributes";
    std::string cur = try_read_file(p).value_or("");
    std::string line = pattern + " filter=lfs diff=lfs merge=lfs -text";
    for (auto& t : tracked_patterns(repo)) if (t == pattern) return;
    if (!cur.empty() && cur.back() != '\n') cur += "\n";
    cur += line + "\n";
    write_file(p, cur);
    repo.invalidate_attributes();
}

std::vector<std::string> tracked_patterns(Repository& repo) {
    std::vector<std::string> out;
    auto d = try_read_file(repo.worktree() / ".gitattributes");
    if (!d) return out;
    size_t pos = 0;
    while (pos < d->size()) {
        size_t nl = d->find('\n', pos);
        std::string line = trim(d->substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? d->size() : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line.find("filter=lfs") != std::string::npos) out.push_back(line.substr(0, line.find_first_of(" \t")));
    }
    return out;
}

}  // namespace lfs
}  // namespace git
