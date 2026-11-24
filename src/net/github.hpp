// SPDX-License-Identifier: MIT
// GitHub REST API client: sign-in (device flow or token), repositories,
// publishing, pull requests.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace gh {

struct Account {
    std::string login, name, email, token, avatar_url, endpoint = "https://api.github.com";
    std::vector<std::string> emails;  // verified emails (for commit attribution)
    int64_t id = 0;
    bool enterprise() const { return endpoint != "https://api.github.com"; }
    std::string host() const;          // github.com or the GHE host
    std::string html_base() const;  // https://github.com
    bool valid() const { return !token.empty(); }
};

struct RepoSummary {
    std::string full_name, owner, name, clone_url, html_url, description, default_branch;
    std::string parent_full_name, parent_clone_url, parent_default_branch;  // forks only (get_repo)
    bool is_private = false, fork = false, archived = false;
    int64_t pushed_at = 0;
};

struct PullRequest {
    int number = 0;
    std::string title, author, head_ref, head_repo_clone, base_ref, html_url;
    bool draft = false;
    int64_t created_at = 0;
};

struct ApiError : std::runtime_error {
    int status;
    ApiError(int s, const std::string& m) : std::runtime_error(m), status(s) {}
};

// OAuth device flow (https://docs.github.com/apps/oauth-apps/building-oauth-apps/authorizing-oauth-apps#device-flow)
struct DeviceCode {
    std::string device_code, user_code, verification_uri;
    int interval = 5, expires_in = 900;
};
DeviceCode start_device_flow(const std::string& client_id);
// Returns the token once the user approved, nullopt while pending, throws on denial/expiry.
std::optional<std::string> poll_device_flow(const std::string& client_id, const DeviceCode& dc, int& interval);

Account fetch_account(const std::string& token, const std::string& endpoint = "https://api.github.com");
std::vector<RepoSummary> list_repos(const Account& a);
std::vector<std::string> list_orgs(const Account& a);
RepoSummary create_repo(const Account& a, const std::string& org, const std::string& name,
                        const std::string& description, bool is_private);
std::vector<PullRequest> list_pull_requests(const Account& a, const std::string& owner, const std::string& repo);

struct CheckRun {
    std::string name, status, conclusion, details_url, app;
    int64_t suite_id = 0, run_id = 0;  // run_id: GitHub Actions workflow run (for re-runs)
    bool failed() const { return conclusion == "failure" || conclusion == "timed_out" || conclusion == "cancelled" || conclusion == "action_required"; }
    bool pending() const { return status != "completed"; }
};
struct Checks {
    std::vector<CheckRun> runs;
    enum State { None, Pending, Success, Failure } state = None;
};
Checks list_checks(const Account& a, const std::string& owner, const std::string& repo, const std::string& sha);
void rerun_failed(const Account& a, const std::string& owner, const std::string& repo, const Checks& c);
RepoSummary get_repo(const Account& a, const std::string& owner, const std::string& repo);
// "@login" -> (name, noreply email)
std::pair<std::string, std::string> coauthor_for_login(const Account& a, const std::string& login);

// Parses GitHub remote URLs into owner/repo (github.com or the given GHE host).
std::optional<std::pair<std::string, std::string>> parse_github_remote(const std::string& url,
                                                                       const std::string& host = "github.com");
int64_t parse_iso8601(const std::string& s);

// ---------------------------------------------------------------- editing on GitHub
// Browse a repository's files over the API and turn edits to any number of files
// into one commit on a new branch plus one pull request.
struct TreeEntry {
    std::string path, sha, mode;  // mode: 100644, 100755, 120000 (symlink), 160000 (submodule)
    int64_t size = 0;
};
struct RemoteTree {
    std::string commit_sha, tree_sha;
    std::vector<TreeEntry> files;  // blobs only, sorted by path
    bool truncated = false;
};
std::vector<std::string> list_branches(const Account& a, const std::string& owner, const std::string& repo);
RemoteTree get_tree(const Account& a, const std::string& owner, const std::string& repo, const std::string& branch);
std::string get_blob(const Account& a, const std::string& owner, const std::string& repo, const std::string& sha);  // raw bytes
bool can_push(const Account& a, const std::string& owner, const std::string& repo);

struct FileEdit {
    std::string path;
    std::optional<std::string> content;  // nullopt: delete the file
    std::string mode = "100644";
    std::string base_sha;                // blob the edit started from ("" for new files), for conflict checks
};
struct PrRequest {
    std::string owner, repo, base_branch, head_branch;
    std::string commit_message, title, body;
    bool draft = false;
    std::vector<FileEdit> edits;
};
struct PrResult {
    int number = 0;
    std::string html_url, head_repo;  // head_repo: owner/repo the branch was pushed to (a fork if needed)
    bool forked = false;
};
// Throws ApiError or std::runtime_error. `progress` gets short status lines.
PrResult submit_pull_request(const Account& a, const PrRequest& req, const std::function<void(const std::string&)>& progress = {});

}  // namespace gh
