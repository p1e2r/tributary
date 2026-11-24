// SPDX-License-Identifier: MIT
// Child processes with pipes (used for git hooks and the SSH transport).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core.hpp"

namespace git {

class Process {
public:
    // argv[0] is looked up on PATH. `env` entries are added to the environment.
    static std::unique_ptr<Process> spawn(const std::vector<std::string>& argv, const fs::path& cwd,
                                          const std::map<std::string, std::string>& env = {},
                                          bool merge_stderr = false);
    virtual ~Process() = default;
    virtual void write(std::string_view data) = 0;
    virtual void close_stdin() = 0;
    virtual size_t read(char* buf, size_t n) = 0;  // stdout; 0 = EOF
    virtual std::string read_stderr_all() = 0;
    virtual int wait() = 0;                          // exit code
};

struct ProcessResult {
    int exit_code = -1;
    std::string output;  // stdout + stderr
};
ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                          const std::map<std::string, std::string>& env = {}, std::string_view input = {});

// Locate an executable on PATH (adds .exe on Windows). Empty if not found.
std::string find_program(const std::string& name);

// Git hooks (.git/hooks/<name>). Returns nullopt when the hook doesn't exist
// or isn't executable; otherwise its result.
std::optional<ProcessResult> run_hook(const fs::path& hooks_dir, const fs::path& gitdir, const fs::path& worktree,
                                      const std::string& name,
                                      const std::vector<std::string>& args = {}, std::string_view input = {});

}  // namespace git
