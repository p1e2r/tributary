// SPDX-License-Identifier: MIT
// TLS via the system OpenSSL (Linux and other Unix systems).
#if !defined(_WIN32) && !defined(__APPLE__)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdlib>

#include "http.hpp"

namespace net {
namespace {

int bio_write(BIO* b, const char* data, int len) {
    auto* s = static_cast<Stream*>(BIO_get_data(b));
    try {
        s->write(data, size_t(len));
        return len;
    } catch (...) {
        return -1;
    }
}

int bio_read(BIO* b, char* data, int len) {
    auto* s = static_cast<Stream*>(BIO_get_data(b));
    try {
        return int(s->read(data, size_t(len)));
    } catch (...) {
        return -1;
    }
}

long bio_ctrl(BIO*, int cmd, long, void*) { return cmd == BIO_CTRL_FLUSH ? 1 : 0; }

BIO_METHOD* method() {
    static BIO_METHOD* m = [] {
        auto* mm = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "tributary");
        BIO_meth_set_write(mm, bio_write);
        BIO_meth_set_read(mm, bio_read);
        BIO_meth_set_ctrl(mm, bio_ctrl);
        BIO_meth_set_create(mm, [](BIO* b) { BIO_set_init(b, 1); return 1; });
        return mm;
    }();
    return m;
}

std::string ssl_err() {
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

class TlsStream : public Stream {
public:
    TlsStream(std::unique_ptr<Stream> raw, const std::string& host) : raw_(std::move(raw)) {
        static SSL_CTX* ctx = [] {
            SSL_CTX* c = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
            SSL_CTX_set_default_verify_paths(c);
            if (const char* f = std::getenv("SSL_CERT_FILE")) SSL_CTX_load_verify_locations(c, f, nullptr);
            for (auto f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                           "/etc/ssl/cert.pem"})
                SSL_CTX_load_verify_locations(c, f, nullptr);
            SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
            return c;
        }();
        ssl_ = SSL_new(ctx);
        BIO* bio = BIO_new(method());
        BIO_set_data(bio, raw_.get());
        SSL_set_bio(ssl_, bio, bio);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) {
            long v = SSL_get_verify_result(ssl_);
            std::string why = v != X509_V_OK ? X509_verify_cert_error_string(v) : ssl_err();
            SSL_free(ssl_);
            ssl_ = nullptr;
            throw NetError("TLS handshake with " + host + " failed: " + why);
        }
    }
    ~TlsStream() override {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
    }
    size_t read(char* buf, size_t n) override {
        int r = SSL_read(ssl_, buf, int(n));
        if (r > 0) return size_t(r);
        int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) return 0;
        throw NetError("TLS read failed: " + ssl_err());
    }
    void write(const char* buf, size_t n) override {
        while (n) {
            int r = SSL_write(ssl_, buf, int(std::min<size_t>(n, 1 << 30)));
            if (r <= 0) throw NetError("TLS write failed: " + ssl_err());
            buf += r;
            n -= size_t(r);
        }
    }

private:
    std::unique_ptr<Stream> raw_;
    SSL* ssl_ = nullptr;
};

}  // namespace

std::unique_ptr<Stream> tls_wrap(std::unique_ptr<Stream> raw, const std::string& host) {
    return std::make_unique<TlsStream>(std::move(raw), host);
}

}  // namespace net
#endif
