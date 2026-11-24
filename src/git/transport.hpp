// SPDX-License-Identifier: MIT
// Git smart-HTTP protocol (v0/v1): ref discovery, fetch (upload-pack) and
// push (receive-pack), plus clone and pull built on top.
#pragma once

#include <functional>

#include "repo.hpp"

namespace git {

struct Credentials {
    std::string username, password;
};
// Called when the server asks for authentication. `attempt` starts at 0 and
// increases after each rejected credential. Return nullopt to cancel.
using CredentialProvider = std::function<std::optional<Credentials>(const std::string& url, int attempt)>;

struct Progress {
    std::string phase;    // "Receiving objects", "Resolving deltas", "Writing objects", ...
    size_t done = 0, total = 0;
    std::string remote_message;  // sideband progress text from the server
};
using ProgressFn = std::function<void(const Progress&)>;

struct RemoteRefs {
    std::vector<std::pair<std::string, ObjectId>> refs;
    std::map<std::string, ObjectId> peeled;
    std::string head_symref;  // e.g. refs/heads/main
    std::set<std::string> caps;
};

RemoteRefs ls_remote(const std::string& url, const std::string& service, const CredentialProvider& creds);

struct FetchResult {
    std::vector<std::string> updated;
    std::string default_branch;
};
FetchResult fetch(Repository& repo, const std::string& remote, const CredentialProvider& creds,
                  const ProgressFn& progress);

// Fetch specific remote refs (e.g. refs/pull/12/head) into local refs.
void fetch_refs(Repository& repo, const std::string& remote,
                const std::vector<std::pair<std::string, std::string>>& src_dst, const CredentialProvider& creds,
                const ProgressFn& progress);

void push(Repository& repo, const std::string& remote, const std::string& branch, const CredentialProvider& creds,
          const ProgressFn& progress, bool set_upstream, bool force = false);
void push_delete(Repository& repo, const std::string& remote, const std::string& branch,
                 const CredentialProvider& creds);
// Delete any ref (branch or tag) on the remote, e.g. "refs/tags/v1".
void push_delete_ref(Repository& repo, const std::string& remote, const std::string& ref, const CredentialProvider& creds);
void push_tags(Repository& repo, const std::string& remote, const CredentialProvider& creds);

Repository clone(const std::string& url, const fs::path& dir, const CredentialProvider& creds,
                 const ProgressFn& progress);

MergeOutcome pull(Repository& repo, const CredentialProvider& creds, const ProgressFn& progress);

// Normalise user input like "owner/repo" or "git@github.com:owner/repo.git".
std::string normalize_clone_url(const std::string& input);
bool is_ssh_url(const std::string& url);

}  // namespace git
