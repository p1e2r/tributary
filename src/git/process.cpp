// SPDX-License-Identifier: MIT
#include "process.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace git {

std::string find_program(const std::string& name) {
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return fs::exists(git_to_path(name)) ? name : "";
    const char* path = std::getenv("PATH");
    if (!path) return "";
#ifdef _WIN32
    char sep = ';';
    std::vector<std::string> exts = {".exe", ".cmd", ".bat", ""};
#else
    char sep = ':';
    std::vector<std::string> exts = {""};
#endif
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(sep, start);
        std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty())
            for (auto& e : exts) {
                fs::path cand = fs::path(dir) / (name + e);
                std::error_code ec;
                if (fs::is_regular_file(cand, ec)) return cand.string();
            }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

#ifdef _WIN32
// ---------------------------------------------------------------- Windows
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { out.append(bs * 2 + 1, L'\\'); bs = 0; out += c; continue; }
        out.append(bs, L'\\');
        bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    return out + L"\"";
}

class WinProcess : public Process {
public:
    HANDLE proc = nullptr, in_w = nullptr, out_r = nullptr, err_r = nullptr;
    ~WinProcess() override {
        if (in_w) CloseHandle(in_w);
        if (out_r) CloseHandle(out_r);
        if (err_r) CloseHandle(err_r);
        if (proc) CloseHandle(proc);
    }
    void write(std::string_view d) override {
        while (!d.empty()) {
            DWORD n = 0;
            if (!WriteFile(in_w, d.data(), DWORD(std::min<size_t>(d.size(), 1 << 20)), &n, nullptr)) throw Error("broken pipe");
            d.remove_prefix(n);
        }
    }
    void close_stdin() override { if (in_w) { CloseHandle(in_w); in_w = nullptr; } }
    size_t read(char* buf, size_t n) override {
        DWORD got = 0;
        if (!ReadFile(out_r, buf, DWORD(n), &got, nullptr)) return 0;
        return got;
    }
    std::string read_stderr_all() override {
        std::string s;
        if (!err_r) return s;
        char buf[4096];
        DWORD got;
        while (ReadFile(err_r, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
        return s;
    }
    int wait() override {
        close_stdin();
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        return int(code);
    }
};

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, const fs::path& cwd,
                                        const std::map<std::string, std::string>& env, bool merge_stderr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w, err_r = nullptr, err_w = nullptr;
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    if (!merge_stderr) CreatePipe(&err_r, &err_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    if (err_r) SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd;
    std::string exe = find_program(argv[0]);
    // A "#!" script (GIT_SSH, hooks, credential helpers): run it with sh like Git for Windows does.
    if (!exe.empty()) {
        std::string ext = to_lower(fs::path(git_to_path(exe)).extension().string());
        if (ext != ".exe" && ext != ".com" && ext != ".bat" && ext != ".cmd") {
            char magic[2] = {};
            if (FILE* f = _wfopen(git_to_path(exe).wstring().c_str(), L"rb")) {
                size_t n = fread(magic, 1, 2, f);
                fclose(f);
                if (n == 2 && magic[0] == '#' && magic[1] == '!') {
                    std::string sh = find_program("sh");
                    if (sh.empty())
                        if (auto g = find_program("git"); !g.empty()) {
                            fs::path root = fs::path(git_to_path(g)).parent_path().parent_path();  // …/Git/cmd/git.exe → …/Git
                            for (auto rel : {"bin/sh.exe", "usr/bin/sh.exe"})
                                if (fs::exists(root / rel)) { sh = (root / rel).string(); break; }
                        }
                    if (!sh.empty()) cmd = quote_arg(widen(sh)) + L" ";
                }
            }
        }
    }
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += L' ';
        cmd += quote_arg(widen(i == 0 && !exe.empty() ? exe : argv[i]));
    }
    // environment block
    std::wstring envblock;
    if (!env.empty()) {
        LPWCH cur = GetEnvironmentStringsW();
        for (LPWCH p = cur; *p; p += wcslen(p) + 1) envblock.append(p).push_back(0);
        FreeEnvironmentStringsW(cur);
        for (auto& [k, v] : env) envblock.append(widen(k + "=" + v)).push_back(0);
        envblock.push_back(0);
    }
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = merge_stderr ? out_w : err_w;
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             envblock.empty() ? nullptr : envblock.data(), wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (err_w) CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        if (err_r) CloseHandle(err_r);
        throw Error("could not run " + argv[0]);
    }
    CloseHandle(pi.hThread);
    auto p = std::make_unique<WinProcess>();
    p->proc = pi.hProcess;
    p->in_w = in_w;
    p->out_r = out_r;
    p->err_r = err_r;
    return p;
}
#else
// ---------------------------------------------------------------- POSIX
class PosixProcess : public Process {
public:
    pid_t pid = -1;
    int in_w = -1, out_r = -1, err_r = -1;
    bool waited = false;
    int code = -1;
    ~PosixProcess() override {
        if (in_w >= 0) ::close(in_w);
        if (out_r >= 0) ::close(out_r);
        if (err_r >= 0) ::close(err_r);
        if (!waited && pid > 0) waitpid(pid, nullptr, 0);
    }
    void write(std::string_view d) override {
        while (!d.empty()) {
            ssize_t n = ::write(in_w, d.data(), d.size());
            if (n < 0) {
                if (errno == EINTR) continue;
                throw Error("broken pipe while talking to child process");
            }
            d.remove_prefix(size_t(n));
        }
    }
    void close_stdin() override { if (in_w >= 0) { ::close(in_w); in_w = -1; } }
    size_t read(char* buf, size_t n) override {
        for (;;) {
            ssize_t r = ::read(out_r, buf, n);
            if (r < 0 && errno == EINTR) continue;
            return r < 0 ? 0 : size_t(r);
        }
    }
    std::string read_stderr_all() override {
        std::string s;
        if (err_r < 0) return s;
        char buf[4096];
        ssize_t r;
        while ((r = ::read(err_r, buf, sizeof buf)) > 0) s.append(buf, size_t(r));
        return s;
    }
    int wait() override {
        close_stdin();
        if (waited) return code;
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        waited = true;
        code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return code;
    }
};

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, const fs::path& cwd,
                                        const std::map<std::string, std::string>& env, bool merge_stderr) {
    int in[2], out[2], err[2] = {-1, -1};
    if (pipe(in) || pipe(out) || (!merge_stderr && pipe(err))) throw Error("pipe failed");
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) throw Error("fork failed");
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(merge_stderr ? out[1] : err[1], 2);
        ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
        if (!merge_stderr) { ::close(err[0]); ::close(err[1]); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    if (!merge_stderr) ::close(err[1]);
    auto p = std::make_unique<PosixProcess>();
    p->pid = pid;
    p->in_w = in[1];
    p->out_r = out[0];
    p->err_r = merge_stderr ? -1 : err[0];
    return p;
}
#endif

ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                          const std::map<std::string, std::string>& env, std::string_view input) {
    auto p = Process::spawn(argv, cwd, env, true);
    std::string in(input);
    std::thread writer([&] {
        try { if (!in.empty()) p->write(in); } catch (...) {}
        p->close_stdin();
    });
    ProcessResult r;
    char buf[8192];
    size_t n;
    while ((n = p->read(buf, sizeof buf)) > 0) r.output.append(buf, n);
    writer.join();
    r.exit_code = p->wait();
    return r;
}

std::optional<ProcessResult> run_hook(const fs::path& hooks_dir, const fs::path& gitdir, const fs::path& worktree,
                                      const std::string& name, const std::vector<std::string>& args, std::string_view input) {
    fs::path hook = hooks_dir / name;
    std::error_code ec;
    if (!fs::is_regular_file(hook, ec)) return std::nullopt;
#ifndef _WIN32
    struct stat st;
    if (::stat(hook.c_str(), &st) != 0 || !(st.st_mode & 0111)) return std::nullopt;
    std::vector<std::string> argv = {hook.string()};
#else
    // Hooks are shell scripts: run them with Git for Windows' sh if present.
    std::string sh = find_program("sh");
    if (sh.empty()) {
        for (auto env : {"ProgramFiles", "ProgramW6432"})
            if (auto pf = std::getenv(env)) {
                auto cand = fs::path(pf) / "Git" / "bin" / "sh.exe";
                if (fs::exists(cand, ec)) { sh = cand.string(); break; }
            }
    }
    if (sh.empty()) return std::nullopt;
    std::vector<std::string> argv = {sh, path_to_git(hook)};
#endif
    for (auto& a : args) argv.push_back(a);
    std::map<std::string, std::string> env = {{"GIT_DIR", path_to_git(gitdir)}, {"GIT_INDEX_FILE", path_to_git(gitdir / "index")}};
    return run_process(argv, worktree, env, input);
}

}  // namespace git
