// SPDX-License-Identifier: MIT
#include "github.hpp"
#include <algorithm>
#include <map>
#include <chrono>
#include <thread>

#include <cstdlib>
#include <ctime>
#include <set>

#include "http.hpp"

namespace gh {

std::string Account::host() const { return net::Url::parse(html_base()).host; }

std::string Account::html_base() const {
    if (endpoint == "https://api.github.com") return "https://github.com";
    auto u = net::Url::parse(endpoint);
    return u.origin();
}

bool starts_with_http(const std::string& s);

static json::Value api(const Account& a, const std::string& method, const std::string& path,
                       const std::string& body = "", std::string* link_header = nullptr) {
    net::Request req;
    req.method = method;
    req.url = starts_with_http(path) ? path : a.endpoint + path;
    req.headers["Accept"] = "application/vnd.github+json";
    req.headers["X-GitHub-Api-Version"] = "2022-11-28";
    if (!a.token.empty()) req.headers["Authorization"] = "Bearer " + a.token;
    if (!body.empty()) {
        req.body = body;
        req.headers["Content-Type"] = "application/json";
    }
    auto r = net::request(req);
    if (link_header) *link_header = r.header("link");
    json::Value v;
    if (!r.body.empty()) {
        try { v = json::parse(r.body); } catch (...) {}
    }
    if (r.status >= 400) {
        std::string msg = v["message"].str("HTTP " + std::to_string(r.status));
        if (v["errors"].is_array() && v["errors"].size()) msg += ": " + v["errors"][0]["message"].str(v["errors"][0]["code"].str());
        throw ApiError(r.status, msg);
    }
    return v;
}

json::Value rest(const Account& a, const std::string& method, const std::string& path, const std::string& body) {
    return api(a, method, path, body);
}

DeviceCode start_device_flow(const std::string& client_id) {
    net::Request req;
    req.method = "POST";
    req.url = "https://github.com/login/device/code";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "client_id=" + net::url_encode(client_id) + "&scope=" + net::url_encode("repo user workflow read:org");
    auto r = net::request(req);
    auto v = json::parse(r.body);
    if (r.status >= 400 || !v["error"].is_null())
        throw ApiError(r.status, v["error_description"].str(v["error"].str("device flow failed")));
    DeviceCode dc;
    dc.device_code = v["device_code"].str();
    dc.user_code = v["user_code"].str();
    dc.verification_uri = v["verification_uri"].str("https://github.com/login/device");
    dc.interval = int(v["interval"].integer(5));
    dc.expires_in = int(v["expires_in"].integer(900));
    return dc;
}

std::optional<std::string> poll_device_flow(const std::string& client_id, const DeviceCode& dc, int& interval) {
    net::Request req;
    req.method = "POST";
    req.url = "https://github.com/login/oauth/access_token";
    req.headers["Accept"] = "application/json";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "client_id=" + net::url_encode(client_id) + "&device_code=" + net::url_encode(dc.device_code) +
               "&grant_type=" + net::url_encode("urn:ietf:params:oauth:grant-type:device_code");
    auto r = net::request(req);
    auto v = json::parse(r.body);
    if (auto tok = v["access_token"].str(); !tok.empty()) return tok;
    auto err = v["error"].str();
    if (err == "authorization_pending") return std::nullopt;
    if (err == "slow_down") { interval += 5; return std::nullopt; }
    throw ApiError(r.status, v["error_description"].str(err.empty() ? "sign-in failed" : err));
}

Account fetch_account(const std::string& token, const std::string& endpoint) {
    Account a;
    a.token = token;
    a.endpoint = endpoint;
    auto v = api(a, "GET", "/user");
    a.login = v["login"].str();
    a.name = v["name"].str(a.login);
    a.email = v["email"].str();
    a.avatar_url = v["avatar_url"].str();
    a.id = v["id"].integer();
    try {
        auto emails = api(a, "GET", "/user/emails");
        for (auto& e : emails.arr()) {
            if (e["verified"].boolean(true)) a.emails.push_back(e["email"].str());
            if (a.email.empty() && e["primary"].boolean()) a.email = e["email"].str();
        }
    } catch (...) {}
    std::string noreply = v["id"].str() + "+" + a.login + "@users.noreply." + (a.enterprise() ? a.host() : "github.com");
    a.emails.push_back(noreply);
    a.emails.push_back(a.login + "@users.noreply.github.com");
    if (a.email.empty()) a.email = noreply;
    return a;
}

static RepoSummary to_repo(const json::Value& r) {
    RepoSummary s;
    s.full_name = r["full_name"].str();
    s.name = r["name"].str();
    s.owner = r["owner"]["login"].str();
    s.clone_url = r["clone_url"].str();
    s.html_url = r["html_url"].str();
    s.description = r["description"].str();
    s.default_branch = r["default_branch"].str("main");
    s.is_private = r["private"].boolean();
    s.fork = r["fork"].boolean();
    s.archived = r["archived"].boolean();
    s.pushed_at = parse_iso8601(r["pushed_at"].str());
    return s;
}

static std::string next_link(const std::string& link) {
    // <https://api.github.com/user/repos?page=2>; rel="next", <...>; rel="last"
    size_t p = 0;
    while (p < link.size()) {
        size_t lt = link.find('<', p), gt = link.find('>', lt);
        if (lt == std::string::npos || gt == std::string::npos) break;
        size_t semi = link.find(',', gt);
        std::string params = link.substr(gt + 1, semi == std::string::npos ? std::string::npos : semi - gt - 1);
        if (params.find("rel=\"next\"") != std::string::npos) return link.substr(lt + 1, gt - lt - 1);
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    return "";
}

std::vector<RepoSummary> list_repos(const Account& a) {
    std::vector<RepoSummary> out;
    std::string url = "/user/repos?per_page=100&sort=pushed&affiliation=owner,collaborator,organization_member";
    for (int page = 0; page < 30 && !url.empty(); page++) {
        std::string link;
        auto v = api(a, "GET", url, "", &link);
        for (auto& r : v.arr()) out.push_back(to_repo(r));
        url = next_link(link);
    }
    return out;
}

std::vector<std::string> list_orgs(const Account& a) {
    std::vector<std::string> out;
    auto v = api(a, "GET", "/user/orgs?per_page=100");
    for (auto& o : v.arr()) out.push_back(o["login"].str());
    return out;
}

RepoSummary create_repo(const Account& a, const std::string& org, const std::string& name,
                        const std::string& description, bool is_private) {
    json::Value body;
    body.set("name", name);
    body.set("description", description);
    body.set("private", is_private);
    body.set("auto_init", false);
    auto v = api(a, "POST", org.empty() ? "/user/repos" : "/orgs/" + org + "/repos", body.dump());
    return to_repo(v);
}

std::vector<PullRequest> list_pull_requests(const Account& a, const std::string& owner, const std::string& repo) {
    std::vector<PullRequest> out;
    auto v = api(a, "GET", "/repos/" + owner + "/" + repo + "/pulls?state=open&per_page=100");
    for (auto& p : v.arr()) {
        PullRequest pr;
        pr.number = int(p["number"].integer());
        pr.title = p["title"].str();
        pr.author = p["user"]["login"].str();
        pr.head_ref = p["head"]["ref"].str();
        pr.head_repo_clone = p["head"]["repo"]["clone_url"].str();
        pr.base_ref = p["base"]["ref"].str();
        pr.html_url = p["html_url"].str();
        pr.draft = p["draft"].boolean();
        pr.created_at = parse_iso8601(p["created_at"].str());
        out.push_back(pr);
    }
    return out;
}

std::optional<std::pair<std::string, std::string>> parse_github_remote(const std::string& url, const std::string& host) {
    std::string rest;
    for (std::string p : {"https://" + host + "/", "http://" + host + "/", "git@" + host + ":", "ssh://git@" + host + "/",
                          "git://" + host + "/"}) {
        if (url.compare(0, p.size(), p) == 0) { rest = url.substr(p.size()); break; }
    }
    if (rest.empty()) {
        // https://user@github.com/owner/repo
        auto at = url.find("@" + host + "/");
        if (at != std::string::npos && url.compare(0, 8, "https://") == 0) rest = url.substr(at + host.size() + 2);
    }
    if (rest.empty()) return std::nullopt;
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.size() > 4 && rest.substr(rest.size() - 4) == ".git") rest.resize(rest.size() - 4);
    auto slash = rest.find('/');
    if (slash == std::string::npos) return std::nullopt;
    return std::make_pair(rest.substr(0, slash), rest.substr(slash + 1));
}

int64_t parse_iso8601(const std::string& s) {
    if (s.size() < 19) return 0;
    std::tm t{};
    t.tm_year = std::stoi(s.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(s.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(s.substr(8, 2));
    t.tm_hour = std::stoi(s.substr(11, 2));
    t.tm_min = std::stoi(s.substr(14, 2));
    t.tm_sec = std::stoi(s.substr(17, 2));
    // days from civil (UTC) without timegm portability issues
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = unsigned(y - era * 400);
    unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

RepoSummary get_repo(const Account& a, const std::string& owner, const std::string& repo) {
    auto v = api(a, "GET", "/repos/" + owner + "/" + repo);
    auto r = to_repo(v);
    if (r.fork && v["parent"].is_object()) {
        r.parent_full_name = v["parent"]["full_name"].str();
        r.parent_clone_url = v["parent"]["clone_url"].str();
        r.parent_default_branch = v["parent"]["default_branch"].str("main");
    }
    return r;
}

Checks list_checks(const Account& a, const std::string& owner, const std::string& repo, const std::string& sha) {
    Checks c;
    auto v = api(a, "GET", "/repos/" + owner + "/" + repo + "/commits/" + sha + "/check-runs?per_page=100");
    for (auto& r : v["check_runs"].arr()) {
        CheckRun cr;
        cr.name = r["name"].str();
        cr.status = r["status"].str();
        cr.conclusion = r["conclusion"].str();
        cr.details_url = r["html_url"].str(r["details_url"].str());
        cr.app = r["app"]["name"].str();
        cr.suite_id = r["check_suite"]["id"].integer();
        auto du = r["details_url"].str();
        auto p = du.find("/actions/runs/");
        if (p != std::string::npos) cr.run_id = std::atoll(du.c_str() + p + 14);
        c.runs.push_back(cr);
    }
    // legacy commit statuses
    try {
        auto st = api(a, "GET", "/repos/" + owner + "/" + repo + "/commits/" + sha + "/status");
        for (auto& s2 : st["statuses"].arr()) {
            CheckRun cr;
            cr.name = s2["context"].str();
            auto state = s2["state"].str();
            cr.status = state == "pending" ? "in_progress" : "completed";
            cr.conclusion = state == "success" ? "success" : state == "pending" ? "" : "failure";
            cr.details_url = s2["target_url"].str();
            c.runs.push_back(cr);
        }
    } catch (...) {}
    if (c.runs.empty()) return c;
    bool any_fail = false, any_pending = false;
    for (auto& r : c.runs) { any_fail |= r.failed(); any_pending |= r.pending(); }
    c.state = any_fail ? Checks::Failure : any_pending ? Checks::Pending : Checks::Success;
    return c;
}

void rerun_failed(const Account& a, const std::string& owner, const std::string& repo, const Checks& c) {
    std::set<int64_t> runs, suites;
    for (auto& r : c.runs) {
        if (!r.failed()) continue;
        if (r.run_id) runs.insert(r.run_id);
        else if (r.suite_id) suites.insert(r.suite_id);
    }
    std::string base = "/repos/" + owner + "/" + repo;
    for (auto id : runs) api(a, "POST", base + "/actions/runs/" + std::to_string(id) + "/rerun-failed-jobs", "{}");
    for (auto id : suites) {
        try { api(a, "POST", base + "/check-suites/" + std::to_string(id) + "/rerequest", "{}"); } catch (...) {}
    }
    if (runs.empty() && suites.empty()) throw ApiError(0, "There are no failed checks that can be re-run.");
}

std::pair<std::string, std::string> coauthor_for_login(const Account& a, const std::string& login) {
    auto v = api(a, "GET", "/users/" + login);
    std::string name = v["name"].str(v["login"].str(login));
    std::string domain = a.enterprise() ? "users.noreply." + a.host() : "users.noreply.github.com";
    return {name, v["id"].str() + "+" + v["login"].str(login) + "@" + domain};
}

bool starts_with_http(const std::string& s) { return s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0; }


// ================================================================ editing on GitHub
static std::string enc_path(const std::string& p) {
    std::string out;
    size_t start = 0;
    while (true) {
        size_t sl = p.find('/', start);
        out += net::url_encode(p.substr(start, sl == std::string::npos ? std::string::npos : sl - start));
        if (sl == std::string::npos) break;
        out += "/";
        start = sl + 1;
    }
    return out;
}

std::vector<std::string> list_branches(const Account& a, const std::string& owner, const std::string& repo) {
    std::vector<std::string> out;
    std::string link, path = "/repos/" + owner + "/" + repo + "/branches?per_page=100";
    for (int page = 0; page < 10 && !path.empty(); page++) {
        auto v = api(a, "GET", path, "", &link);
        for (auto& b : v.arr()) out.push_back(b["name"].str());
        path = next_link(link);
    }
    return out;
}

RemoteTree get_tree(const Account& a, const std::string& owner, const std::string& repo, const std::string& branch) {
    RemoteTree t;
    auto ref = api(a, "GET", "/repos/" + owner + "/" + repo + "/git/ref/heads/" + enc_path(branch));
    t.commit_sha = ref["object"]["sha"].str();
    auto commit = api(a, "GET", "/repos/" + owner + "/" + repo + "/git/commits/" + t.commit_sha);
    t.tree_sha = commit["tree"]["sha"].str();
    auto tree = api(a, "GET", "/repos/" + owner + "/" + repo + "/git/trees/" + t.tree_sha + "?recursive=1");
    t.truncated = tree["truncated"].boolean();
    for (auto& e : tree["tree"].arr()) {
        if (e["type"].str() != "blob") continue;
        TreeEntry te;
        te.path = e["path"].str();
        te.sha = e["sha"].str();
        te.mode = e["mode"].str("100644");
        te.size = e["size"].integer();
        t.files.push_back(std::move(te));
    }
    std::sort(t.files.begin(), t.files.end(), [](auto& x, auto& y) { return x.path < y.path; });
    return t;
}

std::string get_blob(const Account& a, const std::string& owner, const std::string& repo, const std::string& sha) {
    auto v = api(a, "GET", "/repos/" + owner + "/" + repo + "/git/blobs/" + sha);
    std::string b64;
    for (char ch : v["content"].str()) if (ch != '\n' && ch != '\r') b64 += ch;
    return net::base64_decode(b64);
}

bool can_push(const Account& a, const std::string& owner, const std::string& repo) {
    auto v = api(a, "GET", "/repos/" + owner + "/" + repo);
    return v["permissions"]["push"].boolean();
}

PrResult submit_pull_request(const Account& a, const PrRequest& rq, const std::function<void(const std::string&)>& progress) {
    auto say = [&](const std::string& s) { if (progress) progress(s); };
    if (rq.edits.empty()) throw std::runtime_error("There are no changes to submit.");
    std::string up = rq.owner + "/" + rq.repo;
    PrResult res;
    // 1. where the branch goes: the repository itself, or the user's fork of it
    say("Checking permissions…");
    std::string head_repo = up;
    if (!can_push(a, rq.owner, rq.repo)) {
        say("Forking " + up + "…");
        auto fork = api(a, "POST", "/repos/" + up + "/forks", json::Value(json::Object{{"default_branch_only", true}}).dump());
        head_repo = fork["full_name"].str(a.login + "/" + rq.repo);
        // forks are created asynchronously: wait until the API serves it
        for (int i = 0; i < 30; i++) {
            try { api(a, "GET", "/repos/" + head_repo + "/git/ref/heads/" + enc_path(rq.base_branch)); break; }
            catch (const ApiError&) { std::this_thread::sleep_for(std::chrono::seconds(2)); }
        }
        try {
            api(a, "POST", "/repos/" + head_repo + "/merge-upstream", json::Value(json::Object{{"branch", rq.base_branch}}).dump());
        } catch (...) {}
        res.forked = true;
    }
    res.head_repo = head_repo;
    // 2. the latest base commit, and a check that nobody changed our files meanwhile
    say("Reading " + rq.base_branch + "…");
    auto base = get_tree(a, rq.owner, rq.repo, rq.base_branch);
    std::map<std::string, std::string> current;
    for (auto& f : base.files) current[f.path] = f.sha;
    for (auto& e : rq.edits) {
        auto it = current.find(e.path);
        std::string now = it == current.end() ? "" : it->second;
        if (!e.base_sha.empty() && now != e.base_sha)
            throw std::runtime_error(e.path + " was changed on " + rq.base_branch + " after you opened it. Reload the file and apply your edit again.");
        if (e.base_sha.empty() && e.content && !now.empty())
            throw std::runtime_error(e.path + " was created on " + rq.base_branch + " after you started. Pick another name or reload.");
    }
    // 3. blobs + tree + commit
    json::Array entries;
    int n = 0;
    for (auto& e : rq.edits) {
        json::Object te{{"path", e.path}, {"mode", e.mode}, {"type", "blob"}};
        if (e.content) {
            say("Uploading " + std::to_string(++n) + " of " + std::to_string(rq.edits.size()) + "…");
            auto blob = api(a, "POST", "/repos/" + head_repo + "/git/blobs",
                            json::Value(json::Object{{"content", net::base64_encode(*e.content)}, {"encoding", "base64"}}).dump());
            te["sha"] = blob["sha"].str();
        } else {
            te["sha"] = nullptr;  // delete
        }
        entries.push_back(te);
    }
    say("Creating commit…");
    auto tree = api(a, "POST", "/repos/" + head_repo + "/git/trees",
                    json::Value(json::Object{{"base_tree", base.tree_sha}, {"tree", entries}}).dump());
    auto commit = api(a, "POST", "/repos/" + head_repo + "/git/commits",
                      json::Value(json::Object{{"message", rq.commit_message}, {"tree", tree["sha"].str()},
                                               {"parents", json::Array{base.commit_sha}}}).dump());
    // 4. the branch
    say("Creating branch " + rq.head_branch + "…");
    api(a, "POST", "/repos/" + head_repo + "/git/refs",
        json::Value(json::Object{{"ref", "refs/heads/" + rq.head_branch}, {"sha", commit["sha"].str()}}).dump());
    // 5. the pull request
    say("Opening pull request…");
    std::string head = res.forked ? head_repo.substr(0, head_repo.find('/')) + ":" + rq.head_branch : rq.head_branch;
    auto pr = api(a, "POST", "/repos/" + up + "/pulls",
                  json::Value(json::Object{{"title", rq.title}, {"body", rq.body}, {"head", head}, {"base", rq.base_branch},
                                           {"draft", rq.draft}, {"maintainer_can_modify", true}}).dump());
    res.number = int(pr["number"].integer());
    res.html_url = pr["html_url"].str();
    return res;
}

}  // namespace gh
