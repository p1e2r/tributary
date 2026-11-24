// SPDX-License-Identifier: MIT
#include "http.hpp"

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

namespace net {

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

// ================================================================ Url
Url Url::parse(const std::string& s) {
    Url u;
    size_t p = s.find("://");
    if (p == std::string::npos) throw NetError("invalid URL: " + s);
    u.scheme = lower(s.substr(0, p));
    size_t host_start = p + 3;
    size_t path_start = s.find('/', host_start);
    std::string authority = s.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    u.path = path_start == std::string::npos ? "/" : s.substr(path_start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        std::string cred = authority.substr(0, at);
        authority = authority.substr(at + 1);
        size_t colon = cred.find(':');
        u.user = cred.substr(0, colon);
        if (colon != std::string::npos) u.password = cred.substr(colon + 1);
    }
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') u.port = std::stoi(authority.substr(close + 2));
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            u.host = authority.substr(0, colon);
            u.port = std::stoi(authority.substr(colon + 1));
        } else {
            u.host = authority;
        }
    }
    if (!u.port) u.port = u.scheme == "https" ? 443 : 80;
    return u;
}

std::string Url::origin() const {
    bool def = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return scheme + "://" + host + (def ? "" : ":" + std::to_string(port));
}

std::string Url::str() const { return origin() + path; }

// ================================================================ TCP
namespace {
struct WinsockInit {
    WinsockInit() {
#ifdef _WIN32
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
#endif
    }
};

class TcpStream : public Stream {
public:
    explicit TcpStream(sock_t s) : s_(s) {}
    ~TcpStream() override { CLOSESOCK(s_); }
    size_t read(char* buf, size_t n) override {
        for (;;) {
            auto r = ::recv(s_, buf, int(n), 0);
            if (r >= 0) return size_t(r);
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            throw NetError("connection error while reading");
        }
    }
    void write(const char* buf, size_t n) override {
        while (n) {
#ifdef MSG_NOSIGNAL
            auto r = ::send(s_, buf, int(n), MSG_NOSIGNAL);
#else
            auto r = ::send(s_, buf, int(n), 0);
#endif
            if (r <= 0) {
#ifndef _WIN32
                if (r < 0 && errno == EINTR) continue;
#endif
                throw NetError("connection error while writing");
            }
            buf += r;
            n -= size_t(r);
        }
    }

private:
    sock_t s_;
};
}  // namespace

std::unique_ptr<Stream> tcp_connect(const std::string& host, int port) {
    static WinsockInit wi;
    (void)wi;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        throw NetError("could not resolve host " + host);
    sock_t s = INVALID_SOCKET;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, ai->ai_addr, int(ai->ai_addrlen)) == 0) break;
        CLOSESOCK(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) throw NetError("could not connect to " + host + ":" + std::to_string(port));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return std::make_unique<TcpStream>(s);
}

// ================================================================ proxy
std::optional<Url> proxy_for(const Url& target) {
    const char* names[] = {target.scheme == "https" ? "HTTPS_PROXY" : "HTTP_PROXY",
                           target.scheme == "https" ? "https_proxy" : "http_proxy"};
    const char* val = nullptr;
    for (auto n : names) if ((val = std::getenv(n)) && *val) break;
    if (!val || !*val) return std::nullopt;
    const char* np = std::getenv("NO_PROXY");
    if (!np) np = std::getenv("no_proxy");
    if (np) {
        std::string list = np, h = lower(target.host);
        size_t p = 0;
        while (p <= list.size()) {
            size_t c = list.find(',', p);
            std::string item = lower(list.substr(p, c == std::string::npos ? std::string::npos : c - p));
            item.erase(0, item.find_first_not_of(' '));
            while (!item.empty() && item.back() == ' ') item.pop_back();
            if (!item.empty()) {
                if (item == "*") return std::nullopt;
                if (item[0] == '*') item = item.substr(1);
                if (h == item || (item[0] == '.' && h.size() > item.size() && h.compare(h.size() - item.size(), item.size(), item) == 0) ||
                    (item[0] != '.' && h.size() > item.size() && h.compare(h.size() - item.size() - 1, item.size() + 1, "." + item) == 0))
                    return std::nullopt;
                // CIDR-ish prefixes like 127.0.0.0/8 — match loopback/private literally
                if (item.find('/') != std::string::npos) {
                    auto base = item.substr(0, item.find('/'));
                    auto dot = base.find('.');
                    if (dot != std::string::npos && h.compare(0, dot + 1, base.substr(0, dot + 1)) == 0 &&
                        std::all_of(h.begin(), h.end(), [](char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; }))
                        return std::nullopt;
                }
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    std::string v = val;
    if (v.find("://") == std::string::npos) v = "http://" + v;
    return Url::parse(v);
}

// ================================================================ request
namespace {
class Reader {
public:
    explicit Reader(Stream& s) : s_(s) {}
    bool fill() {
        if (pos_ < buf_.size()) return true;
        buf_.resize(65536);
        size_t n = s_.read(buf_.data(), buf_.size());
        buf_.resize(n);
        pos_ = 0;
        return n > 0;
    }
    std::string line() {
        std::string out;
        for (;;) {
            if (!fill()) return out;
            char c = buf_[pos_++];
            if (c == '\n') { if (!out.empty() && out.back() == '\r') out.pop_back(); return out; }
            out += c;
        }
    }
    size_t read(char* dst, size_t n) {
        if (!fill()) return 0;
        size_t k = std::min(n, buf_.size() - pos_);
        std::memcpy(dst, buf_.data() + pos_, k);
        pos_ += k;
        return k;
    }

private:
    Stream& s_;
    std::string buf_;
    size_t pos_ = 0;
};
}  // namespace

static Response do_request(const Request& req, const std::function<void(size_t)>& on_body) {
    Url u = Url::parse(req.url);
    if (u.scheme != "http" && u.scheme != "https") throw NetError("unsupported URL scheme " + u.scheme);
    auto proxy = proxy_for(u);
    std::unique_ptr<Stream> s;
    bool absolute_form = false;
    if (proxy) {
        auto raw = tcp_connect(proxy->host, proxy->port);
        if (u.scheme == "https") {
            std::string connect = "CONNECT " + u.host + ":" + std::to_string(u.port) + " HTTP/1.1\r\nHost: " + u.host +
                                  ":" + std::to_string(u.port) + "\r\n";
            if (!proxy->user.empty())
                connect += "Proxy-Authorization: Basic " + base64_encode(proxy->user + ":" + proxy->password) + "\r\n";
            connect += "\r\n";
            raw->write(connect.data(), connect.size());
            // read the proxy's reply byte-wise so no TLS bytes are consumed
            std::string head;
            char c;
            while (head.size() < 16384 && raw->read(&c, 1) == 1) {
                head += c;
                if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
            }
            auto sp = head.find(' ');
            int code = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
            if (code != 200) throw NetError("proxy refused connection to " + u.host + " (HTTP " + std::to_string(code) + ")");
            s = tls_wrap(std::move(raw), u.host);
        } else {
            s = std::move(raw);
            absolute_form = true;
        }
    } else {
        s = tcp_connect(u.host, u.port);
        if (u.scheme == "https") s = tls_wrap(std::move(s), u.host);
    }

    std::string target = absolute_form ? u.str() : u.path;
    std::string msg = req.method + " " + target + " HTTP/1.1\r\n";
    std::map<std::string, std::string> h;
    h["Host"] = (u.port == 443 || u.port == 80) ? u.host : u.host + ":" + std::to_string(u.port);
    h["User-Agent"] = "git/2.47.0 (Tributary)";
    h["Connection"] = "close";
    h["Accept"] = "*/*";
    if (!u.user.empty()) h["Authorization"] = "Basic " + base64_encode(u.user + ":" + u.password);
    for (auto& [k, v] : req.headers) h[k] = v;
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH")
        h["Content-Length"] = std::to_string(req.body.size());
    for (auto& [k, v] : h) msg += k + ": " + v + "\r\n";
    msg += "\r\n";
    s->write(msg.data(), msg.size());
    if (!req.body.empty()) {
        const size_t chunk = 1 << 16;
        for (size_t off = 0; off < req.body.size(); off += chunk)
            s->write(req.body.data() + off, std::min(chunk, req.body.size() - off));
    }

    Reader rd(*s);
    Response resp;
    resp.final_url = req.url;
    for (;;) {
        std::string status = rd.line();
        if (status.empty()) throw NetError("empty response from " + u.host);
        auto sp = status.find(' ');
        resp.status = sp == std::string::npos ? 0 : std::atoi(status.c_str() + sp + 1);
        resp.headers.clear();
        for (;;) {
            std::string l = rd.line();
            if (l.empty()) break;
            auto colon = l.find(':');
            if (colon == std::string::npos) continue;
            std::string k = lower(l.substr(0, colon));
            std::string v = l.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            resp.headers[k] = v;
        }
        if (resp.status != 100) break;  // skip 100-continue
    }
    if (req.method == "HEAD" || resp.status == 204 || resp.status == 304) return resp;
    std::string te = lower(resp.header("transfer-encoding"));
    char buf[65536];
    if (te.find("chunked") != std::string::npos) {
        for (;;) {
            std::string sz = rd.line();
            size_t n = std::strtoul(sz.c_str(), nullptr, 16);
            if (n == 0) { while (!rd.line().empty()) {} break; }
            while (n) {
                size_t k = rd.read(buf, std::min(n, sizeof buf));
                if (!k) throw NetError("connection closed mid-chunk");
                resp.body.append(buf, k);
                n -= k;
                if (on_body) on_body(resp.body.size());
            }
            rd.line();
        }
    } else if (!resp.header("content-length").empty()) {
        size_t n = std::strtoull(resp.header("content-length").c_str(), nullptr, 10);
        resp.body.reserve(n);
        while (resp.body.size() < n) {
            size_t k = rd.read(buf, std::min(n - resp.body.size(), sizeof buf));
            if (!k) throw NetError("connection closed before the full response arrived");
            resp.body.append(buf, k);
            if (on_body) on_body(resp.body.size());
        }
    } else {
        size_t k;
        while ((k = rd.read(buf, sizeof buf)) > 0) {
            resp.body.append(buf, k);
            if (on_body) on_body(resp.body.size());
        }
    }
    return resp;
}

Response request(const Request& req, const std::function<void(size_t)>& on_body) {
    Request r = req;
    for (int i = 0; i <= req.max_redirects; i++) {
        auto resp = do_request(r, on_body);
        if ((resp.status == 301 || resp.status == 302 || resp.status == 303 || resp.status == 307 || resp.status == 308) &&
            !resp.header("location").empty()) {
            std::string loc = resp.header("location");
            if (loc.find("://") == std::string::npos) loc = Url::parse(r.url).origin() + loc;
            // don't leak credentials to another host
            if (Url::parse(loc).host != Url::parse(r.url).host) r.headers.erase("Authorization");
            if (resp.status == 303 || ((resp.status == 301 || resp.status == 302) && r.method == "POST")) {
                r.method = "GET";
                r.body.clear();
            }
            r.url = loc;
            continue;
        }
        resp.final_url = r.url;
        return resp;
    }
    throw NetError("too many redirects");
}

// ================================================================ encoding
std::string base64_encode(std::string_view in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string base64_decode(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
    }
    return out;
}

std::string url_encode(std::string_view in) {
    std::string out;
    char buf[4];
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
    }
    return out;
}

}  // namespace net
