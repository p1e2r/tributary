// SPDX-License-Identifier: MIT
// Minimal HTTP/1.1 client written from scratch. TLS comes from the operating
// system's TLS library (Schannel / Security.framework / OpenSSL) — see tls_*.cpp.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace net {

struct NetError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Url {
    std::string scheme, host, path, user, password;
    int port = 0;
    static Url parse(const std::string& s);
    std::string origin() const;  // scheme://host[:port]
    std::string str() const;     // without credentials
};

class Stream {
public:
    virtual ~Stream() = default;
    virtual size_t read(char* buf, size_t n) = 0;  // 0 = EOF
    virtual void write(const char* buf, size_t n) = 0;
};

// Plain TCP socket.
std::unique_ptr<Stream> tcp_connect(const std::string& host, int port);
// Wrap a connected stream in TLS (certificate + hostname verified).
std::unique_ptr<Stream> tls_wrap(std::unique_ptr<Stream> raw, const std::string& host);

struct Request {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    int max_redirects = 5;
    // Streaming: called with each piece of the response body as it arrives
    // (server-sent events, NDJSON). Return false to cancel.
    std::function<bool(std::string_view)> on_data;
};

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string final_url;
    std::string header(const std::string& k) const {
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
};

// `on_body` (optional) is called as body bytes arrive (for progress).
Response request(const Request& req, const std::function<void(size_t)>& on_body = {});

std::string base64_encode(std::string_view in);
std::string base64_decode(std::string_view in);
std::string url_encode(std::string_view in);

// Simple process-wide proxy configuration (from HTTPS_PROXY / NO_PROXY).
std::optional<Url> proxy_for(const Url& target);

}  // namespace net
