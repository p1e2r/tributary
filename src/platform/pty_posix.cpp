// SPDX-License-Identifier: MIT
// POSIX pseudo-terminals (macOS, Linux) via posix_openpt; no libutil needed.
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include "platform.hpp"

namespace plat {

namespace {

class PosixPty : public Pty {
public:
    ~PosixPty() override {
        kill();
        if (reader_.joinable()) reader_.join();
        if (master_ >= 0) ::close(master_);
    }

    bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, std::string* error) {
        master_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
            if (error) *error = std::string("couldn't open a pseudo-terminal: ") + std::strerror(errno);
            return false;
        }
        const char* slave_name = ptsname(master_);
        if (!slave_name) { if (error) *error = "ptsname failed"; return false; }
        std::string sname = slave_name;
        winsize ws{};
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        pid_ = fork();
        if (pid_ < 0) { if (error) *error = std::string("fork failed: ") + std::strerror(errno); return false; }
        if (pid_ == 0) {
            // child: new session with the pty as its controlling terminal
            setsid();
            int slave = ::open(sname.c_str(), O_RDWR);
            if (slave < 0) _exit(127);
#ifdef TIOCSCTTY
            ioctl(slave, TIOCSCTTY, 0);
#endif
            ioctl(slave, TIOCSWINSZ, &ws);
            termios t{};
            if (tcgetattr(slave, &t) == 0) {
                t.c_iflag |= IUTF8;
                tcsetattr(slave, TCSANOW, &t);
            }
            dup2(slave, 0);
            dup2(slave, 1);
            dup2(slave, 2);
            if (slave > 2) ::close(slave);
            ::close(master_);
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) {}
            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);
            setenv("TERM_PROGRAM", "Tributary", 1);
            unsetenv("COLUMNS");
            unsetenv("LINES");
            std::vector<char*> args;
            for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
            args.push_back(nullptr);
            signal(SIGINT, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            execvp(args[0], args.data());
            _exit(127);
        }
        int fl = fcntl(master_, F_GETFD);
        fcntl(master_, F_SETFD, fl | FD_CLOEXEC);
        alive_ = true;
        reader_ = std::thread([this] { read_loop(); });
        return true;
    }

    void write(std::string_view data) override {
        size_t off = 0;
        while (off < data.size() && master_ >= 0) {
            ssize_t n = ::write(master_, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
                break;
            }
            off += size_t(n);
        }
    }

    void resize(int cols, int rows) override {
        if (master_ < 0) return;
        winsize ws{};
        ws.ws_col = (unsigned short)cols;
        ws.ws_row = (unsigned short)rows;
        ioctl(master_, TIOCSWINSZ, &ws);
        if (pid_ > 0) ::kill(-pid_, SIGWINCH);
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
        if (pid_ > 0 && alive_) {
            ::kill(-pid_, SIGHUP);
            ::kill(pid_, SIGHUP);
        }
        stop_ = true;
    }

private:
    void read_loop() {
        char tmp[16384];
        while (!stop_) {
            pollfd p{master_, POLLIN, 0};
            int r = poll(&p, 1, 100);
            if (r < 0 && errno == EINTR) continue;
            if (r > 0 && (p.revents & POLLIN)) {
                ssize_t n = ::read(master_, tmp, sizeof tmp);
                if (n > 0) {
                    { std::lock_guard<std::mutex> lk(mu_); buf_.append(tmp, size_t(n)); }
                    if (on_data) on_data();
                    continue;
                }
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                break;  // EIO: the child closed the slave side
            }
            if (r > 0 && (p.revents & (POLLHUP | POLLERR))) break;
            // check whether the child is gone
            int st = 0;
            if (pid_ > 0 && waitpid(pid_, &st, WNOHANG) == pid_) {
                exit_code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
                reaped_ = true;
                break;
            }
        }
        if (!reaped_ && pid_ > 0) {
            int st = 0;
            for (int i = 0; i < 50; i++) {
                if (waitpid(pid_, &st, WNOHANG) == pid_) {
                    exit_code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
                    break;
                }
                usleep(10000);
            }
        }
        alive_ = false;
        if (on_data) on_data();
    }

    int master_ = -1;
    pid_t pid_ = -1;
    std::thread reader_;
    std::mutex mu_;
    std::string buf_;
    std::atomic<bool> alive_{false}, stop_{false};
    std::atomic<int> exit_code_{0};
    bool reaped_ = false;
};

}  // namespace

std::unique_ptr<Pty> Pty::spawn(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, std::string* error) {
    auto p = std::make_unique<PosixPty>();
    if (!p->start(argv, cwd, cols, rows, error)) return nullptr;
    return p;
}

std::vector<std::string> default_shell() {
    const char* sh = std::getenv("SHELL");
    std::string shell = sh && *sh ? sh : "";
    if (shell.empty()) {
        if (auto* pw = getpwuid(getuid()); pw && pw->pw_shell && *pw->pw_shell) shell = pw->pw_shell;
    }
    if (shell.empty()) shell = access("/bin/zsh", X_OK) == 0 ? "/bin/zsh" : "/bin/bash";
    if (access(shell.c_str(), X_OK) != 0) shell = "/bin/sh";
    return {shell, "-l"};  // login shell like Terminal.app and VS Code on macOS
}

}  // namespace plat
