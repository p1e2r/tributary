// Tributary — a from-scratch C++ reimplementation of GitHub Desktop.
// SPDX-License-Identifier: MIT
//
// Core primitives for the git engine: object ids, byte buffers, errors,
// SHA-1, CRC-32, Adler-32 and file-system helpers.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace git {

namespace fs = std::filesystem;
using Bytes = std::string;  // raw byte buffer (std::string is binary-safe)

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- ObjectId
struct ObjectId {
    std::array<uint8_t, 20> b{};

    static ObjectId from_hex(std::string_view hex);
    static std::optional<ObjectId> try_hex(std::string_view hex);
    static ObjectId from_raw(const void* p) {
        ObjectId id;
        std::memcpy(id.b.data(), p, 20);
        return id;
    }
    std::string hex() const;
    std::string short_hex(size_t n = 7) const { return hex().substr(0, n); }
    bool is_zero() const {
        for (auto c : b) if (c) return false;
        return true;
    }
    auto operator<=>(const ObjectId&) const = default;
};

struct ObjectIdHash {
    size_t operator()(const ObjectId& id) const {
        size_t h;
        std::memcpy(&h, id.b.data(), sizeof(h));
        return h;
    }
};

// ---------------------------------------------------------------- hashing
class Sha1 {
public:
    Sha1() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    ObjectId finish();
    static ObjectId of(std::string_view s) {
        Sha1 h;
        h.update(s);
        return h.finish();
    }

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t used_ = 0;
};

// SHA-256 (Git LFS object ids)
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::string finish_hex();
    static std::string hex_of(std::string_view s) {
        Sha256 h;
        h.update(s);
        return h.finish_hex();
    }

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t used_ = 0;
};

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);
uint32_t adler32(const void* data, size_t len, uint32_t a = 1);

// ---------------------------------------------------------------- zlib
// Inflate a zlib stream. `consumed` receives the number of input bytes that
// made up the stream (needed when walking pack files).
Bytes zlib_inflate(const uint8_t* in, size_t in_len, size_t* consumed = nullptr,
                   size_t size_hint = 0);
inline Bytes zlib_inflate(std::string_view in) {
    return zlib_inflate(reinterpret_cast<const uint8_t*>(in.data()), in.size());
}
// Deflate into a zlib stream (LZ77 + Huffman, our own encoder).
Bytes zlib_deflate(std::string_view in, int level = 6);

// ---------------------------------------------------------------- fs
Bytes read_file(const fs::path& p);
std::optional<Bytes> try_read_file(const fs::path& p);
void write_file(const fs::path& p, std::string_view data);
// Write via a temporary file + rename, so readers never see partial data.
void write_file_atomic(const fs::path& p, std::string_view data);

struct FileStat {
    uint32_t ctime_s = 0, ctime_ns = 0, mtime_s = 0, mtime_ns = 0;
    uint32_t dev = 0, ino = 0, mode = 0, uid = 0, gid = 0, size = 0;
    bool is_dir = false, is_file = false, is_link = false, is_exec = false;
};
std::optional<FileStat> stat_path(const fs::path& p);  // lstat semantics

// Lock file à la git: create `<path>.lock` exclusively, write, commit by rename.
class LockFile {
public:
    explicit LockFile(fs::path target);
    ~LockFile();
    void write(std::string_view data);
    void commit();
    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;

private:
    fs::path target_, lock_;
    bool done_ = false;
    std::string data_;
};

// ---------------------------------------------------------------- strings
std::vector<std::string_view> split_lines(std::string_view s);  // keeps no '\n'
std::string trim(std::string_view s);
bool starts_with(std::string_view s, std::string_view p);
std::string to_lower(std::string_view s);
std::string path_to_git(const fs::path& p);  // generic, '/'-separated, UTF-8
fs::path git_to_path(std::string_view s);

int64_t unix_now();
int local_tz_offset_minutes();  // e.g. +420 for UTC+07:00
std::string format_tz(int minutes);  // "+0700"

}  // namespace git
