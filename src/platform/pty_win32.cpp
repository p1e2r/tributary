// SPDX-License-Identifier: MIT
// Windows pseudo-consoles (ConPTY, Windows 10 1809+). The API is looked up at
// runtime so the app still starts on older Windows (the terminal then says so).
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "platform.hpp"

namespace plat {

namespace {

using HPCON_ = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON_*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON_, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON_);
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    ConPtyApi() {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (!k) return;
        create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(k, "CreatePseudoConsole"));
        resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(k, "ResizePseudoConsole"));
        close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(k, "ClosePseudoConsole"));
    }
    bool ok() const { return create && resize && close; }
};
const ConPtyApi& api() {
    static ConPtyApi a;
    return a;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring quote_arg(const std::wstring& a) {
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

class WinPty : public Pty {
public:
    ~WinPty() override {
        kill();
        if (reader_.joinable()) reader_.join();
        if (waiter_.joinable()) waiter_.join();
        if (in_write_) CloseHandle(in_write_);
        if (pi_.hProcess) CloseHandle(pi_.hProcess);
        if (pi_.hThread) CloseHandle(pi_.hThread);
    }

    bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, std::string* error) {
        if (!api().ok()) {
            if (error) *error = "The terminal needs Windows 10 version 1809 or newer (ConPTY).";
            return false;
        }
        HANDLE in_read = nullptr, out_write = nullptr;
        if (!CreatePipe(&in_read, &in_write_, nullptr, 0) || !CreatePipe(&out_read_, &out_write, nullptr, 0)) {
            if (error) *error = "CreatePipe failed";
            return false;
        }
        COORD size{SHORT(cols), SHORT(rows)};
        HRESULT hr = api().create(size, in_read, out_write, 0, &hpc_);
        CloseHandle(in_read);
        CloseHandle(out_write);
        if (FAILED(hr)) {
            if (error) *error = "CreatePseudoConsole failed";
            return false;
        }
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attrs_.resize(attr_size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs_.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(hpc_), nullptr, nullptr)) {
            if (error) *error = "couldn't attach the pseudo-console";
            return false;
        }
        std::wstring cmd;
        for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote_arg(widen(a)); }
        std::wstring wcwd = widen(cwd);
        // the child inherits our environment plus TERM so ssh, git and friends render colors
        SetEnvironmentVariableW(L"TERM", L"xterm-256color");
        SetEnvironmentVariableW(L"TERM_PROGRAM", L"Tributary");
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                 nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi_);
        DeleteProcThreadAttributeList(si.lpAttributeList);
        if (!ok) {
            if (error) *error = "couldn't start " + (argv.empty() ? std::string("the shell") : argv[0]);
            api().close(hpc_);
            hpc_ = nullptr;
            return false;
        }
        alive_ = true;
        reader_ = std::thread([this] { read_loop(); });
        waiter_ = std::thread([this] {
            WaitForSingleObject(pi_.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi_.hProcess, &code);
            exit_code_ = int(code);
            // closing the pseudo-console ends the reader's ReadFile
            if (hpc_) { api().close(hpc_); hpc_ = nullptr; }
        });
        return true;
    }

    void write(std::string_view data) override {
        DWORD w = 0;
        size_t off = 0;
        while (off < data.size() && in_write_) {
            if (!WriteFile(in_write_, data.data() + off, DWORD(data.size() - off), &w, nullptr)) break;
            off += w;
        }
    }

    void resize(int cols, int rows) override {
        if (hpc_) api().resize(hpc_, COORD{SHORT(cols), SHORT(rows)});
    }

    std::string take_output() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string out;
        out.swap(buf_);
        return out;
    }

    bool alive() const override { return alive_.load(); }
    int exit_code() const override { return exit_code_.load(); }

    void kill() override {
        if (alive_ && pi_.hProcess) TerminateProcess(pi_.hProcess, 1);
    }

private:
    void read_loop() {
        char tmp[16384];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(out_read_, tmp, sizeof tmp, &n, nullptr) || n == 0) break;
            { std::lock_guard<std::mutex> lk(mu_); buf_.append(tmp, n); }
            if (on_data) on_data();
        }
        CloseHandle(out_read_);
        out_read_ = nullptr;
        alive_ = false;
        if (on_data) on_data();
    }

    HPCON_ hpc_ = nullptr;
    HANDLE in_write_ = nullptr, out_read_ = nullptr;
    PROCESS_INFORMATION pi_{};
    std::vector<char> attrs_;
    std::thread reader_, waiter_;
    std::mutex mu_;
    std::string buf_;
    std::atomic<bool> alive_{false};
    std::atomic<int> exit_code_{0};
};

}  // namespace

std::unique_ptr<Pty> Pty::spawn(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, std::string* error) {
    auto p = std::make_unique<WinPty>();
    if (!p->start(argv, cwd, cols, rows, error)) return nullptr;
    return p;
}

std::vector<std::string> default_shell() {
    // PowerShell 7 if installed, else Windows PowerShell, else cmd
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return {"pwsh.exe", "-NoLogo"};
    if (SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH, buf, nullptr)) return {"powershell.exe", "-NoLogo"};
    const char* comspec = std::getenv("COMSPEC");
    return {comspec && *comspec ? comspec : "cmd.exe"};
}

}  // namespace plat
