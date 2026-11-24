// SPDX-License-Identifier: MIT
#include "core.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

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
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace git {

// ================================================================ ObjectId
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<ObjectId> ObjectId::try_hex(std::string_view hex) {
    if (hex.size() != 40) return std::nullopt;
    ObjectId id;
    for (int i = 0; i < 20; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        id.b[i] = uint8_t(hi << 4 | lo);
    }
    return id;
}

ObjectId ObjectId::from_hex(std::string_view hex) {
    auto id = try_hex(hex);
    if (!id) throw Error("invalid object id: " + std::string(hex));
    return *id;
}

std::string ObjectId::hex() const {
    static const char* d = "0123456789abcdef";
    std::string s(40, '0');
    for (int i = 0; i < 20; i++) {
        s[2 * i] = d[b[i] >> 4];
        s[2 * i + 1] = d[b[i] & 15];
    }
    return s;
}

// ================================================================ SHA-1
static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)
static const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Sha256::reset() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
    total_ = 0;
    used_ = 0;
}

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    auto p = static_cast<const uint8_t*>(data);
    total_ += len;
    while (len) {
        size_t n = std::min(len, size_t(64) - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n; p += n; len -= n;
        if (used_ == 64) { block(buf_); used_ = 0; }
    }
}

std::string Sha256::finish_hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int s = 28; s >= 0; s -= 4) out += hx[(v >> s) & 15];
    return out;
}

void Sha1::reset() {
    h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
    total_ = 0; used_ = 0;
}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 |
               uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    auto p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (used_) {
        size_t n = std::min(len, 64 - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n; p += n; len -= n;
        if (used_ == 64) { block(buf_); used_ = 0; }
    }
    while (len >= 64) { block(p); p += 64; len -= 64; }
    if (len) { std::memcpy(buf_, p, len); used_ = len; }
}

ObjectId Sha1::finish() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    ObjectId id;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) id.b[4 * i + j] = uint8_t(h_[i] >> (24 - 8 * j));
    reset();
    return id;
}

// ================================================================ CRC / Adler
uint32_t crc32(const void* data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    auto p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const void* data, size_t len, uint32_t adler) {
    auto p = static_cast<const uint8_t*>(data);
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (len) {
        size_t n = std::min<size_t>(len, 5552);
        len -= n;
        while (n--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return b << 16 | a;
}

// ================================================================ files
Bytes read_file(const fs::path& p) {
    auto r = try_read_file(p);
    if (!r) throw Error("cannot read " + path_to_git(p));
    return std::move(*r);
}

std::optional<Bytes> try_read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, std::string_view data) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("cannot write " + path_to_git(p));
    f.write(data.data(), std::streamsize(data.size()));
    if (!f) throw Error("short write to " + path_to_git(p));
}

void write_file_atomic(const fs::path& p, std::string_view data) {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path tmp = p;
    tmp += ".tmp" + std::to_string(rng() % 1000000);
    write_file(tmp, data);
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {  // Windows can refuse to replace; fall back
        fs::remove(p, ec);
        fs::rename(tmp, p);
    }
}

std::optional<FileStat> stat_path(const fs::path& p) {
    FileStat s;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return std::nullopt;
    auto conv = [](FILETIME ft, uint32_t& sec, uint32_t& ns) {
        uint64_t t = (uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime);
        t -= 116444736000000000ULL;  // 1601 -> 1970
        sec = uint32_t(t / 10000000ULL);
        ns = uint32_t((t % 10000000ULL) * 100);
    };
    conv(a.ftCreationTime, s.ctime_s, s.ctime_ns);
    conv(a.ftLastWriteTime, s.mtime_s, s.mtime_ns);
    s.is_dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    s.is_link = a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
    s.is_file = !s.is_dir;
    s.size = a.nFileSizeLow;
    s.mode = s.is_dir ? 0040000 : 0100644;
#else
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return std::nullopt;
#ifdef __APPLE__
    s.ctime_s = uint32_t(st.st_ctimespec.tv_sec); s.ctime_ns = uint32_t(st.st_ctimespec.tv_nsec);
    s.mtime_s = uint32_t(st.st_mtimespec.tv_sec); s.mtime_ns = uint32_t(st.st_mtimespec.tv_nsec);
#else
    s.ctime_s = uint32_t(st.st_ctim.tv_sec); s.ctime_ns = uint32_t(st.st_ctim.tv_nsec);
    s.mtime_s = uint32_t(st.st_mtim.tv_sec); s.mtime_ns = uint32_t(st.st_mtim.tv_nsec);
#endif
    s.dev = uint32_t(st.st_dev); s.ino = uint32_t(st.st_ino);
    s.uid = uint32_t(st.st_uid); s.gid = uint32_t(st.st_gid);
    s.size = uint32_t(st.st_size);
    s.is_dir = S_ISDIR(st.st_mode);
    s.is_file = S_ISREG(st.st_mode);
    s.is_link = S_ISLNK(st.st_mode);
    s.is_exec = s.is_file && (st.st_mode & 0100);
    s.mode = s.is_link ? 0120000 : s.is_dir ? 0040000 : s.is_exec ? 0100755 : 0100644;
#endif
    return s;
}

LockFile::LockFile(fs::path target) : target_(std::move(target)) {
    lock_ = target_;
    lock_ += ".lock";
    if (lock_.has_parent_path()) fs::create_directories(lock_.parent_path());
#ifdef _WIN32
    HANDLE h = CreateFileW(lock_.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error("'" + path_to_git(lock_) + "' exists: another git process may be running");
    CloseHandle(h);
#else
    int fd = ::open(lock_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
        throw Error("'" + path_to_git(lock_) + "' exists: another git process may be running");
    ::close(fd);
#endif
}

LockFile::~LockFile() {
    if (!done_) {
        std::error_code ec;
        fs::remove(lock_, ec);
    }
}

void LockFile::write(std::string_view data) { data_.append(data); }

void LockFile::commit() {
    {
        std::ofstream f(lock_, std::ios::binary | std::ios::trunc);
        f.write(data_.data(), std::streamsize(data_.size()));
        if (!f) throw Error("cannot write " + path_to_git(lock_));
    }
    std::error_code ec;
    fs::rename(lock_, target_, ec);
    if (ec) {
        fs::remove(target_, ec);
        fs::rename(lock_, target_);
    }
    done_ = true;
}

// ================================================================ strings
std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return std::string(s.substr(a, b - a));
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return r;
}

std::string path_to_git(const fs::path& p) {
    auto u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path git_to_path(std::string_view s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int local_tz_offset_minutes() {
    std::time_t t = std::time(nullptr);
    std::tm lt{}, gt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
    gmtime_s(&gt, &t);
#else
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
#endif
    gt.tm_isdst = lt.tm_isdst;
    return int(std::difftime(std::mktime(&lt), std::mktime(&gt)) / 60);
}

std::string format_tz(int m) {
    char buf[16];
    int a = m < 0 ? -m : m;
    std::snprintf(buf, sizeof buf, "%c%02d%02d", m < 0 ? '-' : '+', a / 60, a % 60);
    return buf;
}

}  // namespace git
