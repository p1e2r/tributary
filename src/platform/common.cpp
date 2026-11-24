// SPDX-License-Identifier: MIT
// Shared, mostly-portable pieces of the platform layer.
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "platform.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace plat {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
#ifdef _WIN32
    std::wstring cmd;
    for (auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"' + widen(a) + L'"';
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            std::vector<char*> args;
            for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
#endif
}

static std::vector<std::string> split_cmd(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (char c : s) {
        if (c == '"') { q = !q; continue; }
        if (c == ' ' && !q) { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    spawn_detached({"open", url});
#else
    spawn_detached({"xdg-open", url});
#endif
}

void reveal_in_file_manager(const std::string& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"explore", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    spawn_detached({"open", path});
#else
    spawn_detached({"xdg-open", path});
#endif
}

void open_in_terminal(const std::string& path, const std::string& custom) {
    if (!custom.empty()) {
        auto args = split_cmd(custom);
        args.push_back(path);
        spawn_detached(args);
        return;
    }
#ifdef _WIN32
    // Windows Terminal if present, else PowerShell
    std::wstring wt = widen("wt.exe -d \"" + path + "\"");
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, wt.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    ShellExecuteW(nullptr, L"open", L"powershell.exe", L"-NoExit", widen(path).c_str(), SW_SHOWNORMAL);
#elif defined(__APPLE__)
    spawn_detached({"open", "-a", "Terminal", path});
#else
    for (auto t : {"x-terminal-emulator", "gnome-terminal", "konsole", "xfce4-terminal", "alacritty", "kitty", "xterm"}) {
        std::string probe = std::string("command -v ") + t + " >/dev/null 2>&1";
        if (std::system(probe.c_str()) == 0) {
            if (std::string(t) == "gnome-terminal") spawn_detached({t, "--working-directory=" + path});
            else if (std::string(t) == "konsole") spawn_detached({t, "--workdir", path});
            else spawn_detached({"sh", "-c", "cd \"$0\" && exec " + std::string(t), path});
            return;
        }
    }
#endif
}

void open_in_editor(const std::string& path, const std::string& custom) {
    if (!custom.empty()) {
        auto args = split_cmd(custom);
        args.push_back(path);
        spawn_detached(args);
        return;
    }
#ifdef __APPLE__
    // prefer VS Code if installed, else the default app
    if (fs::exists("/Applications/Visual Studio Code.app")) spawn_detached({"open", "-a", "Visual Studio Code", path});
    else if (fs::exists("/Applications/Zed.app")) spawn_detached({"open", "-a", "Zed", path});
    else spawn_detached({"open", path});
#elif defined(_WIN32)
    spawn_detached({"cmd.exe", "/c", "code", path});
#else
    if (std::system("command -v code >/dev/null 2>&1") == 0) spawn_detached({"code", path});
    else if (std::system("command -v zed >/dev/null 2>&1") == 0) spawn_detached({"zed", path});
    else spawn_detached({"xdg-open", path});
#endif
}

std::string app_data_dir() {
    fs::path p;
#ifdef _WIN32
    PWSTR w = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &w))) {
        p = fs::path(w) / "Tributary";
        CoTaskMemFree(w);
    }
#elif defined(__APPLE__)
    p = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / "Library" / "Application Support" / "Tributary";
#else
    if (auto x = std::getenv("XDG_CONFIG_HOME")) p = fs::path(x) / "tributary";
    else p = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config" / "tributary";
#endif
    std::error_code ec;
    fs::create_directories(p, ec);
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string default_clone_dir() {
    fs::path home;
#ifdef _WIN32
    PWSTR w = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &w))) {
        home = fs::path(w);
        CoTaskMemFree(w);
    }
#else
    home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / "Documents";
#endif
    auto u8 = (home / "GitHub").u8string();
    return std::string(u8.begin(), u8.end());
}

std::string os_name() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

// ================================================================ trash
#ifdef __APPLE__
}  // namespace plat
#include <objc/message.h>
#include <objc/runtime.h>
namespace plat {
bool move_to_trash(const std::string& path) {
    using id = ::id;
    auto cls = [](const char* n) { return reinterpret_cast<id>(objc_getClass(n)); };
    auto sel = [](const char* n) { return sel_registerName(n); };
    id str = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend)(cls("NSString"), sel("stringWithUTF8String:"), path.c_str());
    id url = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(cls("NSURL"), sel("fileURLWithPath:"), str);
    id fm = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls("NSFileManager"), sel("defaultManager"));
    return reinterpret_cast<signed char (*)(id, SEL, id, id*, id*)>(objc_msgSend)(fm, sel("trashItemAtURL:resultingItemURL:error:"), url, nullptr, nullptr);
}
#elif defined(_WIN32)
bool move_to_trash(const std::string& path) {
    std::wstring w = widen(path);
    for (auto& c : w) if (c == L'/') c = L'\\';
    w.push_back(0);
    w.push_back(0);
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = w.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
#else
// freedesktop.org trash specification
bool move_to_trash(const std::string& path) {
    fs::path src(path);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(src, ec))) return false;
    fs::path base = std::getenv("XDG_DATA_HOME") ? fs::path(std::getenv("XDG_DATA_HOME"))
                                                 : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".local" / "share";
    fs::path files = base / "Trash" / "files", info = base / "Trash" / "info";
    fs::create_directories(files, ec);
    fs::create_directories(info, ec);
    std::string name = src.filename().string();
    std::string unique = name;
    for (int i = 2; fs::exists(files / unique, ec) || fs::exists(info / (unique + ".trashinfo"), ec); i++)
        unique = name + "." + std::to_string(i);
    std::time_t t = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::string abs = fs::absolute(src, ec).string();
    std::string enc;
    for (unsigned char c : abs) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') enc += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); enc += b; }
    }
    {
        FILE* f = std::fopen((info / (unique + ".trashinfo")).c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc.c_str(), date);
        std::fclose(f);
    }
    fs::rename(src, files / unique, ec);
    if (ec) {  // different filesystem: copy then remove
        ec.clear();
        fs::copy(src, files / unique, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return false;
        fs::remove_all(src, ec);
    }
    return true;
}
#endif

// ================================================================ secrets
#ifdef __APPLE__
}  // namespace plat
#include <Security/Security.h>
namespace plat {
static CFStringRef cfs(const std::string& s) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()), kCFStringEncodingUTF8, false);
}
static CFMutableDictionaryRef keychain_query(const std::string& key) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef svc = cfs("Tributary"), acct = cfs(key);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(svc);
    CFRelease(acct);
    return q;
}
std::string secret_get(const std::string& key) {
    auto q = keychain_query(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFTypeRef out = nullptr;
    std::string s;
    if (SecItemCopyMatching(q, &out) == errSecSuccess && out) {
        auto d = static_cast<CFDataRef>(out);
        s.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d)));
        CFRelease(out);
    }
    CFRelease(q);
    return s;
}
void secret_set(const std::string& key, const std::string& value) {
    secret_delete(key);
    auto q = keychain_query(key);
    CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(value.data()), CFIndex(value.size()));
    CFDictionarySetValue(q, kSecValueData, d);
    SecItemAdd(q, nullptr);
    CFRelease(d);
    CFRelease(q);
}
void secret_delete(const std::string& key) {
    auto q = keychain_query(key);
    SecItemDelete(q);
    CFRelease(q);
}
#elif defined(_WIN32)
}  // namespace plat
#include <wincred.h>
#pragma comment(lib, "advapi32.lib")
namespace plat {
std::string secret_get(const std::string& key) {
    PCREDENTIALW cred = nullptr;
    std::wstring target = widen("Tributary:" + key);
    std::string s;
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        s.assign(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        CredFree(cred);
    }
    return s;
}
void secret_set(const std::string& key, const std::string& value) {
    std::wstring target = widen("Tributary:" + key), user = widen(key);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.CredentialBlobSize = DWORD(value.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.UserName = user.data();
    CredWriteW(&c, 0);
}
void secret_delete(const std::string& key) {
    std::wstring target = widen("Tributary:" + key);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}
#else
// Linux: a 0600 file in the config directory (no desktop keyring dependency).
static fs::path secret_file(const std::string& key) {
    std::string safe;
    for (char c : key) safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return fs::path(app_data_dir()) / "secrets" / safe;
}
std::string secret_get(const std::string& key) {
    FILE* f = std::fopen(secret_file(key).c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
void secret_set(const std::string& key, const std::string& value) {
    auto p = secret_file(key);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::permissions(p.parent_path(), fs::perms::owner_all, ec);
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    [[maybe_unused]] auto w = ::write(fd, value.data(), value.size());
    ::close(fd);
}
void secret_delete(const std::string& key) {
    std::error_code ec;
    fs::remove(secret_file(key), ec);
}
#endif

std::string arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string executable_path() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    auto u8 = fs::path(buf).u8string();
    return std::string(u8.begin(), u8.end());
#elif defined(__APPLE__)
    char exe[PATH_MAX];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) != 0) return "";
    std::error_code ec;
    auto p = fs::weakly_canonical(exe, ec);
    return ec ? std::string(exe) : p.string();
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "" : p.string();
#endif
}

PFN_vkGetInstanceProcAddr load_vulkan() {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    if (!m) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"));
#else
    std::vector<std::string> names;
#ifdef __APPLE__
    // Look next to the executable first (bundled in Contents/Frameworks)
    char exe[PATH_MAX];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        auto dir = fs::path(exe).parent_path();
        for (auto n : {"../Frameworks/libvulkan.1.dylib", "../Frameworks/libMoltenVK.dylib", "libvulkan.1.dylib", "libMoltenVK.dylib"})
            names.push_back((dir / n).string());
    }
    names.push_back("libvulkan.1.dylib");
    names.push_back("libMoltenVK.dylib");
    names.push_back("/usr/local/lib/libvulkan.1.dylib");
    names.push_back("/opt/homebrew/lib/libvulkan.1.dylib");
    names.push_back("/opt/homebrew/lib/libMoltenVK.dylib");
#else
    names = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (auto& n : names) {
        if (void* h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            if (auto f = dlsym(h, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkGetInstanceProcAddr>(f);
        }
    }
    return nullptr;
#endif
}

}  // namespace plat
