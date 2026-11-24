// SPDX-License-Identifier: MIT
// Reading .tar.gz / .tar / .zip archives (used by the self-updater), with our own inflate.
#pragma once

#include <string>
#include <vector>

#include "core.hpp"

namespace git {

struct ArchiveEntry {
    std::string path;     // '/'-separated, relative
    Bytes data;
    uint32_t mode = 0644; // unix permission bits
    bool dir = false, symlink = false;  // symlink: data is the target
};

Bytes gunzip(std::string_view gz);
std::vector<ArchiveEntry> read_tar(std::string_view tar);
std::vector<ArchiveEntry> read_zip(std::string_view zip);
// Detects the format by its magic bytes.
std::vector<ArchiveEntry> read_archive(std::string_view data);
// Writes entries under `dir`, refusing paths that escape it. Returns the top-level names written.
std::vector<std::string> extract_archive(std::string_view data, const fs::path& dir);

}  // namespace git
