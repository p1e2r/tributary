// SPDX-License-Identifier: MIT
// TLS via Windows Schannel (SSPI).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <cstring>
#include <vector>

#include "http.hpp"

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

namespace net {
namespace {

class TlsStream : public Stream {
public:
    TlsStream(std::unique_ptr<Stream> raw, const std::string& host) : raw_(std::move(raw)) {
        whost_ = std::wstring(host.begin(), host.end());
        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS;
        cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
        TimeStamp ts;
        if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                      &cred, nullptr, nullptr, &cred_, &ts) != SEC_E_OK) {
            // TLS 1.3 may be unsupported on older Windows: retry with 1.2 only
            cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
            if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                          &cred, nullptr, nullptr, &cred_, &ts) != SEC_E_OK)
                throw NetError("could not acquire TLS credentials");
        }
        have_cred_ = true;
        handshake();
        QueryContextAttributes(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
    }
    ~TlsStream() override {
        if (have_ctx_) DeleteSecurityContext(&ctx_);
        if (have_cred_) FreeCredentialsHandle(&cred_);
    }

    size_t read(char* buf, size_t n) override {
        while (plain_pos_ >= plain_.size()) {
            if (closed_) return 0;
            plain_.clear();
            plain_pos_ = 0;
            decrypt_more();
        }
        size_t k = std::min(n, plain_.size() - plain_pos_);
        std::memcpy(buf, plain_.data() + plain_pos_, k);
        plain_pos_ += k;
        return k;
    }

    void write(const char* buf, size_t n) override {
        while (n) {
            size_t chunk = std::min<size_t>(n, sizes_.cbMaximumMessage);
            std::vector<char> msg(sizes_.cbHeader + chunk + sizes_.cbTrailer);
            std::memcpy(msg.data() + sizes_.cbHeader, buf, chunk);
            SecBuffer b[4];
            b[0] = {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, msg.data()};
            b[1] = {ULONG(chunk), SECBUFFER_DATA, msg.data() + sizes_.cbHeader};
            b[2] = {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, msg.data() + sizes_.cbHeader + chunk};
            b[3] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) throw NetError("TLS encrypt failed");
            raw_->write(msg.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
            buf += chunk;
            n -= chunk;
        }
    }

private:
    void recv_more() {
        char tmp[16384];
        size_t r = raw_->read(tmp, sizeof tmp);
        if (r == 0) throw NetError("connection closed during TLS");
        in_.insert(in_.end(), tmp, tmp + r);
    }

    void handshake() {
        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;
        DWORD out_flags;
        TimeStamp ts;
        bool first = true;
        for (;;) {
            SecBuffer inb[2] = {{ULONG(in_.size()), SECBUFFER_TOKEN, in_.empty() ? nullptr : in_.data()},
                                {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc ind{SECBUFFER_VERSION, 2, inb};
            SecBuffer outb[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc outd{SECBUFFER_VERSION, 1, outb};
            SECURITY_STATUS s = InitializeSecurityContextW(
                &cred_, first ? nullptr : &ctx_, first ? const_cast<wchar_t*>(whost_.c_str()) : nullptr, flags, 0, 0,
                first ? nullptr : &ind, 0, first ? &ctx_ : nullptr, &outd, &out_flags, &ts);
            if (first) have_ctx_ = true;
            first = false;
            if (outb[0].cbBuffer && outb[0].pvBuffer) {
                raw_->write(static_cast<char*>(outb[0].pvBuffer), outb[0].cbBuffer);
                FreeContextBuffer(outb[0].pvBuffer);
            }
            if (s == SEC_E_INCOMPLETE_MESSAGE) { recv_more(); continue; }
            // keep any extra (unprocessed) bytes
            if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
                std::vector<char> extra(in_.end() - inb[1].cbBuffer, in_.end());
                in_ = std::move(extra);
            } else if (s != SEC_E_INCOMPLETE_MESSAGE) {
                in_.clear();
            }
            if (s == SEC_E_OK) return;
            if (s == SEC_I_CONTINUE_NEEDED) {
                if (in_.empty()) recv_more();
                continue;
            }
            if (s == SEC_I_INCOMPLETE_CREDENTIALS) continue;
            throw NetError("TLS handshake failed (Schannel error 0x" + [&] {
                char b[16];
                snprintf(b, sizeof b, "%08lx", (unsigned long)s);
                return std::string(b);
            }() + ")");
        }
    }

    void decrypt_more() {
        for (;;) {
            if (!in_.empty()) {
                SecBuffer b[4] = {{ULONG(in_.size()), SECBUFFER_DATA, in_.data()},
                                  {0, SECBUFFER_EMPTY, nullptr},
                                  {0, SECBUFFER_EMPTY, nullptr},
                                  {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d{SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS s = DecryptMessage(&ctx_, &d, 0, nullptr);
                if (s == SEC_E_OK || s == SEC_I_RENEGOTIATE || s == SEC_I_CONTEXT_EXPIRED) {
                    std::vector<char> extra;
                    for (auto& x : b) {
                        if (x.BufferType == SECBUFFER_DATA && x.cbBuffer)
                            plain_.append(static_cast<char*>(x.pvBuffer), x.cbBuffer);
                        if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer)
                            extra.assign(static_cast<char*>(x.pvBuffer), static_cast<char*>(x.pvBuffer) + x.cbBuffer);
                    }
                    in_ = std::move(extra);
                    if (s == SEC_I_CONTEXT_EXPIRED) { closed_ = true; return; }
                    if (s == SEC_I_RENEGOTIATE) { handshake(); }
                    if (!plain_.empty()) return;
                    continue;
                }
                if (s != SEC_E_INCOMPLETE_MESSAGE) throw NetError("TLS decrypt failed");
            }
            char tmp[16384];
            size_t r = raw_->read(tmp, sizeof tmp);
            if (r == 0) { closed_ = true; return; }
            in_.insert(in_.end(), tmp, tmp + r);
        }
    }

    std::unique_ptr<Stream> raw_;
    std::wstring whost_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool have_cred_ = false, have_ctx_ = false, closed_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<char> in_;
    std::string plain_;
    size_t plain_pos_ = 0;
};

}  // namespace

std::unique_ptr<Stream> tls_wrap(std::unique_ptr<Stream> raw, const std::string& host) {
    return std::make_unique<TlsStream>(std::move(raw), host);
}

}  // namespace net
#endif
