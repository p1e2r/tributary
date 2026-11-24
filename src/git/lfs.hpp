// SPDX-License-Identifier: MIT
// Git LFS: pointer files, the local object store, and the batch API
// (https://github.com/git-lfs/git-lfs/blob/main/docs/api/batch.md).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "transport.hpp"

namespace git::lfs {

struct Pointer {
    std::string oid;  // sha256 hex
    uint64_t size = 0;
    bool operator<(const Pointer& o) const { return oid < o.oid; }
};

std::optional<Pointer> parse_pointer(std::string_view data);
std::string make_pointer(const Pointer& p);

// Download objects that aren't in .git/lfs/objects yet.
void download(Repository& repo, const std::string& remote, const std::vector<Pointer>& objects, const CredentialProvider& creds,
              const ProgressFn& progress = {});
// Upload objects the server doesn't have (the batch API tells us which).
void upload(Repository& repo, const std::string& remote, const std::vector<Pointer>& objects, const CredentialProvider& creds,
            const ProgressFn& progress = {}, const std::string& ref = "");

// Make the worktree whole: download every LFS object HEAD needs, then replace
// pointer files in the worktree with the real content. Returns files replaced.
size_t pull(Repository& repo, const std::string& remote, const CredentialProvider& creds, const ProgressFn& progress = {});
// Pointers among a set of objects (blobs being pushed).
std::vector<Pointer> pointers_in(Repository& repo, const std::set<ObjectId>& objects);
// Track a pattern in .gitattributes ("*.psd filter=lfs diff=lfs merge=lfs -text").
void track(Repository& repo, const std::string& pattern);
std::vector<std::string> tracked_patterns(Repository& repo);

}  // namespace git::lfs
