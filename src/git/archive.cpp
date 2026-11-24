// SPDX-License-Identifier: MIT
#include "archive.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace git {

static uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
static uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

Bytes gunzip(std::string_view gz) {
    auto p = reinterpret_cast<const uint8_t*>(gz.data());
    size_t n = gz.size();
    Bytes out;
    size_t pos = 0;
    while (pos + 18 <= n && p[pos] == 0x1f && p[pos + 1] == 0x8b) {  // concatenated members
        if (p[pos + 2] != 8) throw Error("gzip: unknown compression method");
        uint8_t flg = p[pos + 3];
        size_t i = pos + 10;
        if (flg & 4) { if (i + 2 > n) throw Error("gzip: truncated"); i += 2 + le16(p + i); }
        if (flg & 8) { while (i < n && p[i]) i++; i++; }
        if (flg & 16) { while (i < n && p[i]) i++; i++; }
        if (flg & 2) i += 2;
        if (i >= n) throw Error("gzip: truncated header");
        size_t used = 0;
        Bytes part = raw_inflate(p + i, n - i, &used);
        i += used;
        if (i + 8 > n) throw Error("gzip: missing trailer");
        if (crc32(part.data(), part.size()) != le32(p + i)) throw Error("gzip: checksum mismatch");
        if (uint32_t(part.size()) != le32(p + i + 4)) throw Error("gzip: size mismatch");
        out += part;
        pos = i + 8;
    }
    if (pos == 0) throw Error("not a gzip file");
    return out;
}

static uint64_t octal(const char* s, size_t n) {
    uint64_t v = 0;
    if (n && (uint8_t(s[0]) & 0x80)) {  // base-256 (GNU, big files)
        for (size_t i = 1; i < n; i++) v = v << 8 | uint8_t(s[i]);
        return v;
    }
    for (size_t i = 0; i < n && s[i]; i++)
        if (s[i] >= '0' && s[i] <= '7') v = v * 8 + uint64_t(s[i] - '0');
    return v;
}

static std::string field(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

std::vector<ArchiveEntry> read_tar(std::string_view tar) {
    std::vector<ArchiveEntry> out;
    size_t pos = 0;
    std::string long_name, long_link;
    while (pos + 512 <= tar.size()) {
        const char* h = tar.data() + pos;
        if (std::all_of(h, h + 512, [](char c) { return c == 0; })) break;
        uint64_t size = octal(h + 124, 12);
        char type = h[156];
        size_t data = pos + 512;
        if (data + size > tar.size()) throw Error("tar: truncated");
        std::string_view body = tar.substr(data, size_t(size));
        pos = data + ((size_t(size) + 511) / 512) * 512;
        if (type == 'L') { long_name = std::string(body.substr(0, body.find('\0'))); continue; }
        if (type == 'K') { long_link = std::string(body.substr(0, body.find('\0'))); continue; }
        if (type == 'x') {  // pax: "len key=value\n" records
            size_t i = 0;
            while (i < body.size()) {
                size_t sp = body.find(' ', i);
                if (sp == std::string_view::npos) break;
                size_t len = std::stoul(std::string(body.substr(i, sp - i)));
                std::string_view rec = body.substr(sp + 1, len - (sp + 1 - i) - 1);
                if (rec.substr(0, 5) == "path=") long_name = std::string(rec.substr(5));
                if (rec.substr(0, 9) == "linkpath=") long_link = std::string(rec.substr(9));
                i += len;
            }
            continue;
        }
        if (type == 'g') continue;
        ArchiveEntry e;
        std::string name = field(h, 100);
        if (std::string_view(h + 257, 5) == "ustar") {
            std::string prefix = field(h + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        if (!long_name.empty()) name = long_name;
        e.path = name;
        e.mode = uint32_t(octal(h + 100, 8)) & 07777;
        if (type == '5') e.dir = true;
        else if (type == '2') { e.symlink = true; e.data = long_link.empty() ? field(h + 157, 100) : long_link; }
        else if (type == '0' || type == '\0' || type == '7') e.data = Bytes(body);
        else { long_name.clear(); long_link.clear(); continue; }  // devices, hard links…
        long_name.clear();
        long_link.clear();
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<ArchiveEntry> read_zip(std::string_view zip) {
    auto p = reinterpret_cast<const uint8_t*>(zip.data());
    size_t n = zip.size();
    if (n < 22) throw Error("zip: too short");
    // end of central directory: last 22..65557 bytes
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > (n > 65557 ? n - 65557 : 0);)
        if (le32(p + i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) throw Error("zip: no central directory");
    size_t count = le16(p + eocd + 10), cd = le32(p + eocd + 16);
    std::vector<ArchiveEntry> out;
    for (size_t k = 0; k < count; k++) {
        if (cd + 46 > n || le32(p + cd) != 0x02014b50) throw Error("zip: bad central directory");
        uint32_t made_by = le16(p + cd + 4), method = le16(p + cd + 10), csize = le32(p + cd + 20), usize = le32(p + cd + 24);
        uint32_t nlen = le16(p + cd + 28), xlen = le16(p + cd + 30), clen = le16(p + cd + 32);
        uint32_t ext = le32(p + cd + 38), local = le32(p + cd + 42);
        ArchiveEntry e;
        e.path = std::string(zip.substr(cd + 46, nlen));
        std::replace(e.path.begin(), e.path.end(), '\\', '/');
        cd += 46 + nlen + xlen + clen;
        uint32_t unix_mode = (made_by >> 8) == 3 ? ext >> 16 : 0;
        e.dir = !e.path.empty() && e.path.back() == '/';
        if (e.dir) e.path.pop_back();
        if ((unix_mode & 0170000) == 0040000) e.dir = true;
        e.symlink = (unix_mode & 0170000) == 0120000;
        e.mode = unix_mode ? (unix_mode & 07777) : (e.dir ? 0755 : 0644);
        if (!e.dir) {
            if (size_t(local) + 30 > n || le32(p + local) != 0x04034b50) throw Error("zip: bad local header");
            size_t data = local + 30 + le16(p + local + 26) + le16(p + local + 28);
            if (data + csize > n) throw Error("zip: truncated");
            if (method == 0) e.data = Bytes(zip.substr(data, csize));
            else if (method == 8) e.data = raw_inflate(p + data, csize, nullptr, usize);
            else throw Error("zip: unsupported compression method " + std::to_string(method));
            if (e.data.size() != usize) throw Error("zip: size mismatch in " + e.path);
            if (crc32(e.data.data(), e.data.size()) != le32(p + cd - (46 + nlen + xlen + clen) + 16)) throw Error("zip: checksum mismatch in " + e.path);
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<ArchiveEntry> read_archive(std::string_view data) {
    if (data.size() >= 2 && uint8_t(data[0]) == 0x1f && uint8_t(data[1]) == 0x8b) {
        Bytes tar = gunzip(data);
        return read_tar(tar);
    }
    if (data.size() >= 4 && data.substr(0, 4) == std::string_view("PK\x03\x04", 4)) return read_zip(data);
    if (data.size() >= 512 && data.substr(257, 5) == "ustar") return read_tar(data);
    throw Error("unknown archive format");
}

std::vector<std::string> extract_archive(std::string_view data, const fs::path& dir) {
    auto entries = read_archive(data);
    std::set<std::string> tops;
    fs::create_directories(dir);
    for (auto& e : entries) {
        // normalize and refuse anything that escapes `dir`
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= e.path.size()) {
            size_t j = e.path.find('/', i);
            if (j == std::string::npos) j = e.path.size();
            std::string seg = e.path.substr(i, j - i);
            if (seg == "..") throw Error("archive entry escapes the folder: " + e.path);
            if (!seg.empty() && seg != ".") parts.push_back(seg);
            i = j + 1;
        }
        if (parts.empty()) continue;
        if (parts[0] == "__MACOSX") continue;
        tops.insert(parts[0]);
        fs::path target = dir;
        for (auto& s : parts) target /= git_to_path(s);
        if (e.dir) { fs::create_directories(target); continue; }
        fs::create_directories(target.parent_path());
        std::error_code ec;
        fs::remove(target, ec);
        if (e.symlink) {
#ifndef _WIN32
            if (e.data.find("..") != std::string::npos && e.data.find('/') == 0) throw Error("bad symlink in archive");
            fs::create_symlink(git_to_path(e.data), target);
#endif
            continue;
        }
        write_file(target, e.data);
#ifndef _WIN32
        ::chmod(target.c_str(), mode_t(e.mode & 0777));
#endif
    }
    return {tops.begin(), tops.end()};
}

}  // namespace git
