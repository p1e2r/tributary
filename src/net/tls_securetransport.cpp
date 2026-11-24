// SPDX-License-Identifier: MIT
// TLS via Apple's Security.framework (SecureTransport) on macOS.
#ifdef __APPLE__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/SecureTransport.h>
#include <Security/Security.h>

#include "http.hpp"

namespace net {
namespace {

OSStatus st_read(SSLConnectionRef conn, void* data, size_t* len) {
    auto* s = static_cast<Stream*>(const_cast<void*>(conn));
    size_t want = *len, got = 0;
    try {
        while (got < want) {
            size_t r = s->read(static_cast<char*>(data) + got, want - got);
            if (r == 0) { *len = got; return got ? noErr : errSSLClosedGraceful; }
            got += r;
        }
    } catch (...) {
        *len = got;
        return errSSLClosedAbort;
    }
    *len = got;
    return noErr;
}

OSStatus st_write(SSLConnectionRef conn, const void* data, size_t* len) {
    auto* s = static_cast<Stream*>(const_cast<void*>(conn));
    try {
        s->write(static_cast<const char*>(data), *len);
    } catch (...) {
        *len = 0;
        return errSSLClosedAbort;
    }
    return noErr;
}

class TlsStream : public Stream {
public:
    TlsStream(std::unique_ptr<Stream> raw, const std::string& host) : raw_(std::move(raw)) {
        ctx_ = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
        if (!ctx_) throw NetError("could not create TLS context");
        SSLSetIOFuncs(ctx_, st_read, st_write);
        SSLSetConnection(ctx_, raw_.get());
        SSLSetPeerDomainName(ctx_, host.data(), host.size());
        SSLSetProtocolVersionMin(ctx_, kTLSProtocol12);
        OSStatus s;
        do { s = SSLHandshake(ctx_); } while (s == errSSLWouldBlock);
        if (s != noErr) {
            CFRelease(ctx_);
            ctx_ = nullptr;
            throw NetError("TLS handshake with " + host + " failed (OSStatus " + std::to_string(int(s)) + ")");
        }
    }
    ~TlsStream() override {
        if (ctx_) {
            SSLClose(ctx_);
            CFRelease(ctx_);
        }
    }
    size_t read(char* buf, size_t n) override {
        size_t got = 0;
        OSStatus s = SSLRead(ctx_, buf, n, &got);
        if (got > 0) return got;
        if (s == errSSLClosedGraceful || s == errSSLClosedNoNotify || s == errSSLClosedAbort) return 0;
        if (s != noErr && s != errSSLWouldBlock) throw NetError("TLS read failed (OSStatus " + std::to_string(int(s)) + ")");
        return got;
    }
    void write(const char* buf, size_t n) override {
        while (n) {
            size_t done = 0;
            OSStatus s = SSLWrite(ctx_, buf, n, &done);
            if (s != noErr && s != errSSLWouldBlock) throw NetError("TLS write failed");
            buf += done;
            n -= done;
        }
    }

private:
    std::unique_ptr<Stream> raw_;
    SSLContextRef ctx_ = nullptr;
};

}  // namespace

std::unique_ptr<Stream> tls_wrap(std::unique_ptr<Stream> raw, const std::string& host) {
    return std::make_unique<TlsStream>(std::move(raw), host);
}

}  // namespace net
#endif
