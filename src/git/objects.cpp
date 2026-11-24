// SPDX-License-Identifier: MIT
#include "objects.hpp"

#include <algorithm>
#include <functional>

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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace git {

const char* type_name(ObjType t) {
    switch (t) {
        case ObjType::Commit: return "commit";
        case ObjType::Tree: return "tree";
        case ObjType::Blob: return "blob";
        case ObjType::Tag: return "tag";
        default: return "unknown";
    }
}

ObjType type_from_name(std::string_view s) {
    if (s == "commit") return ObjType::Commit;
    if (s == "tree") return ObjType::Tree;
    if (s == "blob") return ObjType::Blob;
    if (s == "tag") return ObjType::Tag;
    return ObjType::None;
}

Bytes object_header(ObjType t, size_t size) {
    return std::string(type_name(t)) + " " + std::to_string(size) + '\0';
}

ObjectId hash_object(ObjType t, std::string_view data) {
    Sha1 h;
    h.update(object_header(t, data.size()));
    h.update(data);
    return h.finish();
}

// ================================================================ Signature
Signature Signature::parse(std::string_view s) {
    Signature sig;
    size_t lt = s.find('<'), gt = s.rfind('>');
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt < lt) {
        sig.name = std::string(s);
        return sig;
    }
    sig.name = trim(s.substr(0, lt));
    sig.email = std::string(s.substr(lt + 1, gt - lt - 1));
    std::string rest = trim(s.substr(gt + 1));
    size_t sp = rest.find(' ');
    try {
        sig.time = std::stoll(rest.substr(0, sp));
        if (sp != std::string::npos) {
            std::string tz = rest.substr(sp + 1);
            if (tz.size() >= 5) {
                int v = std::stoi(tz.substr(1, 2)) * 60 + std::stoi(tz.substr(3, 2));
                sig.tz = tz[0] == '-' ? -v : v;
            }
        }
    } catch (...) {
    }
    return sig;
}

std::string Signature::format() const {
    return name + " <" + email + "> " + std::to_string(time) + " " + format_tz(tz);
}

Signature Signature::now(std::string name, std::string email) {
    return {std::move(name), std::move(email), unix_now(), local_tz_offset_minutes()};
}

// ================================================================ Commit
Commit Commit::parse(std::string_view d) {
    Commit c;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t nl = d.find('\n', pos);
        if (nl == std::string_view::npos) nl = d.size();
        std::string_view line = d.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            c.message = std::string(pos <= d.size() ? d.substr(pos) : std::string_view{});
            break;
        }
        size_t sp = line.find(' ');
        std::string_view key = line.substr(0, sp), val = sp == std::string_view::npos ? "" : line.substr(sp + 1);
        if (key == "tree") c.tree = ObjectId::from_hex(val);
        else if (key == "parent") c.parents.push_back(ObjectId::from_hex(val));
        else if (key == "author") c.author = Signature::parse(val);
        else if (key == "committer") c.committer = Signature::parse(val);
        else {
            std::string v(val);
            // continuation lines start with a space
            while (pos < d.size() && d[pos] == ' ') {
                size_t n2 = d.find('\n', pos);
                if (n2 == std::string_view::npos) n2 = d.size();
                v += '\n';
                v += d.substr(pos + 1, n2 - pos - 1);
                pos = n2 + 1;
            }
            c.extra_headers.emplace_back(std::string(key), v);
        }
    }
    return c;
}

Bytes Commit::serialize() const {
    std::string s = "tree " + tree.hex() + "\n";
    for (auto& p : parents) s += "parent " + p.hex() + "\n";
    s += "author " + author.format() + "\n";
    s += "committer " + committer.format() + "\n";
    for (auto& [k, v] : extra_headers) {
        s += k + " ";
        for (char ch : v) {
            s += ch;
            if (ch == '\n') s += ' ';
        }
        s += "\n";
    }
    s += "\n" + message;
    return s;
}

std::string Commit::summary() const {
    size_t nl = message.find('\n');
    return trim(message.substr(0, nl));
}

std::string Commit::body() const {
    size_t nl = message.find('\n');
    if (nl == std::string::npos) return "";
    return trim(message.substr(nl + 1));
}

// ================================================================ Tree
Tree Tree::parse(std::string_view d) {
    Tree t;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t sp = d.find(' ', pos);
        size_t nul = d.find('\0', sp);
        if (sp == std::string_view::npos || nul == std::string_view::npos || nul + 21 > d.size())
            throw Error("corrupt tree object");
        TreeEntry e;
        e.mode = uint32_t(std::stoul(std::string(d.substr(pos, sp - pos)), nullptr, 8));
        e.name = std::string(d.substr(sp + 1, nul - sp - 1));
        e.id = ObjectId::from_raw(d.data() + nul + 1);
        t.entries.push_back(std::move(e));
        pos = nul + 21;
    }
    return t;
}

static bool tree_less(const TreeEntry& a, const TreeEntry& b) {
    std::string ka = a.name + (a.is_tree() ? "/" : "");
    std::string kb = b.name + (b.is_tree() ? "/" : "");
    return ka < kb;
}

Bytes Tree::serialize() const {
    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end(), tree_less);
    std::string s;
    for (auto& e : sorted) {
        char mode[16];
        std::snprintf(mode, sizeof mode, "%o", e.mode);
        s += mode;
        s += ' ';
        s += e.name;
        s += '\0';
        s.append(reinterpret_cast<const char*>(e.id.b.data()), 20);
    }
    return s;
}

const TreeEntry* Tree::find(std::string_view name) const {
    for (auto& e : entries) if (e.name == name) return &e;
    return nullptr;
}

// ================================================================ Tag
Tag Tag::parse(std::string_view d) {
    Tag t;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t nl = d.find('\n', pos);
        if (nl == std::string_view::npos) nl = d.size();
        std::string_view line = d.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) { t.message = std::string(pos <= d.size() ? d.substr(pos) : ""); break; }
        size_t sp = line.find(' ');
        auto key = line.substr(0, sp);
        auto val = sp == std::string_view::npos ? std::string_view{} : line.substr(sp + 1);
        if (key == "object") t.object = ObjectId::from_hex(val);
        else if (key == "type") t.type = type_from_name(val);
        else if (key == "tag") t.name = std::string(val);
        else if (key == "tagger") t.tagger = Signature::parse(val);
    }
    return t;
}

Bytes Tag::serialize() const {
    return "object " + object.hex() + "\ntype " + type_name(type) + "\ntag " + name +
           "\ntagger " + tagger.format() + "\n\n" + message;
}

// ================================================================ MappedFile
MappedFile::MappedFile(const fs::path& p) {
#ifdef _WIN32
    HANDLE f = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) throw Error("cannot open " + path_to_git(p));
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    size_ = size_t(sz.QuadPart);
    if (size_ == 0) { CloseHandle(f); data_ = reinterpret_cast<const uint8_t*>(""); return; }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { CloseHandle(f); throw Error("cannot map " + path_to_git(p)); }
    data_ = static_cast<const uint8_t*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
    file_ = f;
    map_ = m;
    if (!data_) throw Error("cannot map " + path_to_git(p));
#else
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) throw Error("cannot open " + path_to_git(p));
    struct stat st;
    fstat(fd, &st);
    size_ = size_t(st.st_size);
    if (size_ == 0) { ::close(fd); data_ = reinterpret_cast<const uint8_t*>(""); return; }
    void* m = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        fallback_ = read_file(p);
        data_ = reinterpret_cast<const uint8_t*>(fallback_.data());
    } else {
        data_ = static_cast<const uint8_t*>(m);
    }
#endif
}

MappedFile::~MappedFile() {
    if (!size_) return;
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
    if (map_) CloseHandle(map_);
    if (file_) CloseHandle(file_);
#else
    if (fallback_.empty()) munmap(const_cast<uint8_t*>(data_), size_);
#endif
}

// ================================================================ delta
static uint64_t read_varint_le(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t c = *p++;
        v |= uint64_t(c & 0x7F) << shift;
        shift += 7;
        if (!(c & 0x80)) break;
    }
    return v;
}

Bytes apply_delta(std::string_view base, std::string_view delta) {
    auto p = reinterpret_cast<const uint8_t*>(delta.data());
    auto end = p + delta.size();
    uint64_t src_size = read_varint_le(p, end);
    uint64_t dst_size = read_varint_le(p, end);
    if (src_size != base.size()) throw Error("delta base size mismatch");
    Bytes out;
    out.reserve(dst_size);
    while (p < end) {
        uint8_t op = *p++;
        if (op & 0x80) {
            uint32_t off = 0, sz = 0;
            for (int i = 0; i < 4; i++) if (op & (1 << i)) off |= uint32_t(*p++) << (8 * i);
            for (int i = 0; i < 3; i++) if (op & (0x10 << i)) sz |= uint32_t(*p++) << (8 * i);
            if (sz == 0) sz = 0x10000;
            if (size_t(off) + sz > base.size()) throw Error("delta copy out of range");
            out.append(base.substr(off, sz));
        } else if (op) {
            if (p + op > end) throw Error("delta insert out of range");
            out.append(reinterpret_cast<const char*>(p), op);
            p += op;
        } else {
            throw Error("delta opcode 0");
        }
    }
    if (out.size() != dst_size) throw Error("delta result size mismatch");
    return out;
}

// ================================================================ Pack
static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

Pack::Pack(const fs::path& pack, const fs::path& idx) {
    idx_ = std::make_unique<MappedFile>(idx);
    pack_ = std::make_unique<MappedFile>(pack);
    const uint8_t* d = idx_->data();
    if (idx_->size() < 8 + 1024 || be32(d) != 0xff744f63 || be32(d + 4) != 2)
        throw Error("unsupported pack index " + path_to_git(idx));
    fanout_ = d + 8;
    count_ = be32(fanout_ + 255 * 4);
    ids_ = fanout_ + 1024;
    const uint8_t* crcs = ids_ + 20 * count_;
    offs32_ = crcs + 4 * count_;
    offs64_ = offs32_ + 4 * count_;
    if (pack_->size() < 12 || std::memcmp(pack_->data(), "PACK", 4) != 0)
        throw Error("bad pack file " + path_to_git(pack));
}

ObjectId Pack::id_at(size_t i) const { return ObjectId::from_raw(ids_ + 20 * i); }

std::vector<ObjectId> Pack::all_ids() const {
    std::vector<ObjectId> v;
    v.reserve(count_);
    for (size_t i = 0; i < count_; i++) v.push_back(id_at(i));
    return v;
}

std::optional<uint64_t> Pack::find(const ObjectId& id) const {
    uint8_t first = id.b[0];
    size_t lo = first ? be32(fanout_ + (first - 1) * 4) : 0;
    size_t hi = be32(fanout_ + first * 4);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::memcmp(ids_ + 20 * mid, id.b.data(), 20);
        if (c == 0) {
            uint32_t o = be32(offs32_ + 4 * mid);
            if (o & 0x80000000u) {
                const uint8_t* q = offs64_ + 8 * (o & 0x7FFFFFFFu);
                return uint64_t(be32(q)) << 32 | be32(q + 4);
            }
            return o;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return std::nullopt;
}

void Pack::match_prefix(std::string_view hex, std::vector<ObjectId>& out) const {
    for (size_t i = 0; i < count_; i++) {
        // cheap filter on first byte
        auto id = id_at(i);
        if (id.hex().compare(0, hex.size(), hex) == 0) out.push_back(id);
    }
}

Object Pack::read_at(uint64_t offset,
                     const std::function<std::optional<Object>(const ObjectId&)>& resolve_ref) const {
    {
        std::lock_guard lk(cache_mu_);
        auto it = cache_.find(offset);
        if (it != cache_.end()) return *it->second;
    }
    const uint8_t* base = pack_->data();
    size_t size = pack_->size();
    if (offset >= size) throw Error("pack offset out of range");
    const uint8_t* p = base + offset;
    const uint8_t* end = base + size;
    uint8_t c = *p++;
    auto type = ObjType((c >> 4) & 7);
    uint64_t sz = c & 15;
    int shift = 4;
    while (c & 0x80) {
        c = *p++;
        sz |= uint64_t(c & 0x7F) << shift;
        shift += 7;
    }
    Object result;
    if (type == ObjType::OfsDelta || type == ObjType::RefDelta) {
        Object baseobj;
        if (type == ObjType::OfsDelta) {
            c = *p++;
            uint64_t neg = c & 0x7F;
            while (c & 0x80) {
                c = *p++;
                neg = ((neg + 1) << 7) | (c & 0x7F);
            }
            baseobj = read_at(offset - neg, resolve_ref);
        } else {
            auto bid = ObjectId::from_raw(p);
            p += 20;
            if (auto o = find(bid)) baseobj = read_at(*o, resolve_ref);
            else if (auto ext = resolve_ref(bid)) baseobj = std::move(*ext);
            else throw Error("missing delta base " + bid.hex());
        }
        Bytes delta = zlib_inflate(p, size_t(end - p), nullptr, size_t(sz));
        result.type = baseobj.type;
        result.data = apply_delta(baseobj.data, delta);
    } else {
        result.type = type;
        result.data = zlib_inflate(p, size_t(end - p), nullptr, size_t(sz));
    }
    {
        std::lock_guard lk(cache_mu_);
        if (result.data.size() < (4 << 20)) {
            if (cache_order_.size() > 256) {
                cache_.erase(cache_order_.front());
                cache_order_.erase(cache_order_.begin());
            }
            cache_[offset] = std::make_shared<Object>(result);
            cache_order_.push_back(offset);
        }
    }
    return result;
}

// ================================================================ index-pack
ObjectId index_pack(std::string_view pack_data, const fs::path& pack_dir,
                    const std::function<std::optional<Object>(const ObjectId&)>& external,
                    const std::function<void(size_t, size_t)>& progress) {
    auto d = reinterpret_cast<const uint8_t*>(pack_data.data());
    size_t n = pack_data.size();
    if (n < 32 || std::memcmp(d, "PACK", 4) != 0) throw Error("received data is not a pack");
    uint32_t count = be32(d + 8);
    {
        Sha1 h;
        h.update(d, n - 20);
        if (std::memcmp(h.finish().b.data(), d + n - 20, 20) != 0) throw Error("pack checksum mismatch");
    }
    struct Ent {
        uint64_t off;
        uint32_t crc;
        ObjType type;
        uint64_t base_off = 0;  // for ofs delta
        ObjectId base_id;       // for ref delta
        Bytes payload;          // inflated (object or delta)
        bool resolved = false;
        Object obj;
        ObjectId id;
    };
    std::vector<Ent> ents(count);
    size_t pos = 12;
    for (uint32_t i = 0; i < count; i++) {
        Ent& e = ents[i];
        e.off = pos;
        const uint8_t* p = d + pos;
        uint8_t c = *p++;
        e.type = ObjType((c >> 4) & 7);
        uint64_t sz = c & 15;
        int shift = 4;
        while (c & 0x80) { c = *p++; sz |= uint64_t(c & 0x7F) << shift; shift += 7; }
        if (e.type == ObjType::OfsDelta) {
            c = *p++;
            uint64_t neg = c & 0x7F;
            while (c & 0x80) { c = *p++; neg = ((neg + 1) << 7) | (c & 0x7F); }
            e.base_off = e.off - neg;
        } else if (e.type == ObjType::RefDelta) {
            e.base_id = ObjectId::from_raw(p);
            p += 20;
        }
        size_t used = 0;
        e.payload = zlib_inflate(p, size_t(d + n - 20 - p), &used, size_t(sz));
        p += used;
        size_t next = size_t(p - d);
        e.crc = crc32(d + pos, next - pos);
        pos = next;
        if (progress && (i % 256 == 0)) progress(i, count);
    }
    std::unordered_map<uint64_t, size_t> by_off;
    for (size_t i = 0; i < ents.size(); i++) by_off[ents[i].off] = i;
    std::unordered_map<ObjectId, size_t, ObjectIdHash> by_id;
    // resolve base objects first
    for (size_t i = 0; i < ents.size(); i++) {
        auto& e = ents[i];
        if (e.type != ObjType::OfsDelta && e.type != ObjType::RefDelta) {
            e.obj.type = e.type;
            e.obj.data = std::move(e.payload);
            e.id = hash_object(e.type, e.obj.data);
            e.resolved = true;
            by_id[e.id] = i;
        }
    }
    // iterate until all deltas resolved
    std::vector<ObjectId> thin_bases;
    bool progress_made = true;
    while (progress_made) {
        progress_made = false;
        for (size_t i = 0; i < ents.size(); i++) {
            auto& e = ents[i];
            if (e.resolved) continue;
            const Object* base = nullptr;
            Object ext;
            if (e.type == ObjType::OfsDelta) {
                auto it = by_off.find(e.base_off);
                if (it == by_off.end()) throw Error("bad ofs-delta base");
                if (!ents[it->second].resolved) continue;
                base = &ents[it->second].obj;
            } else {
                auto it = by_id.find(e.base_id);
                if (it != by_id.end()) base = &ents[it->second].obj;
                else if (auto o = external(e.base_id)) { ext = std::move(*o); base = &ext; }
                else continue;
            }
            e.obj.type = base->type;
            e.obj.data = apply_delta(base->data, e.payload);
            e.payload.clear();
            e.payload.shrink_to_fit();
            e.id = hash_object(e.obj.type, e.obj.data);
            e.resolved = true;
            by_id[e.id] = i;
            progress_made = true;
        }
    }
    for (auto& e : ents) if (!e.resolved) throw Error("could not resolve all deltas in pack");

    Bytes final_pack(pack_data);
    ObjectId pack_sum = ObjectId::from_raw(d + n - 20);

    // write idx v2
    std::vector<size_t> order(ents.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ents[a].id < ents[b].id; });
    Bytes idx;
    auto put32 = [&](uint32_t v) {
        idx.push_back(char(v >> 24)); idx.push_back(char(v >> 16));
        idx.push_back(char(v >> 8)); idx.push_back(char(v));
    };
    put32(0xff744f63);
    put32(2);
    uint32_t fan[256]{};
    for (auto& e : ents) fan[e.id.b[0]]++;
    uint32_t acc = 0;
    for (int i = 0; i < 256; i++) { acc += fan[i]; put32(acc); }
    for (auto i : order) idx.append(reinterpret_cast<const char*>(ents[i].id.b.data()), 20);
    for (auto i : order) put32(ents[i].crc);
    std::vector<uint64_t> big;
    for (auto i : order) {
        if (ents[i].off >= 0x80000000ull) { put32(0x80000000u | uint32_t(big.size())); big.push_back(ents[i].off); }
        else put32(uint32_t(ents[i].off));
    }
    for (auto o : big) { put32(uint32_t(o >> 32)); put32(uint32_t(o)); }
    idx.append(reinterpret_cast<const char*>(pack_sum.b.data()), 20);
    Sha1 ih;
    ih.update(idx);
    auto isum = ih.finish();
    idx.append(reinterpret_cast<const char*>(isum.b.data()), 20);

    fs::create_directories(pack_dir);
    auto base = pack_dir / ("pack-" + pack_sum.hex());
    write_file_atomic(fs::path(base).concat(".pack"), final_pack);
    write_file_atomic(fs::path(base).concat(".idx"), idx);
    if (progress) progress(count, count);
    return pack_sum;
}

Bytes build_pack(const std::vector<std::pair<ObjectId, Object>>& objects) {
    Bytes out = "PACK";
    auto put32 = [&](uint32_t v) {
        out.push_back(char(v >> 24)); out.push_back(char(v >> 16));
        out.push_back(char(v >> 8)); out.push_back(char(v));
    };
    put32(2);
    put32(uint32_t(objects.size()));
    for (auto& [id, obj] : objects) {
        uint64_t sz = obj.data.size();
        uint8_t c = uint8_t((int(obj.type) << 4) | (sz & 15));
        sz >>= 4;
        while (sz) {
            out.push_back(char(c | 0x80));
            c = uint8_t(sz & 0x7F);
            sz >>= 7;
        }
        out.push_back(char(c));
        out += zlib_deflate(obj.data, 1);
    }
    Sha1 h;
    h.update(out);
    auto sum = h.finish();
    out.append(reinterpret_cast<const char*>(sum.b.data()), 20);
    return out;
}

// ================================================================ ObjectDB
ObjectDB::ObjectDB(fs::path objects_dir) : dir_(std::move(objects_dir)) {
    reload_packs();
    if (auto alt = try_read_file(dir_ / "info" / "alternates")) {
        for (auto line : split_lines(*alt)) {
            auto t = trim(line);
            if (t.empty() || t[0] == '#') continue;
            fs::path p = git_to_path(t);
            if (p.is_relative()) p = dir_ / p;
            if (fs::exists(p)) alt_dbs_.push_back(std::make_unique<ObjectDB>(p));
        }
    }
}

void ObjectDB::reload_packs() {
    std::lock_guard lk(mu_);
    packs_.clear();
    std::error_code ec;
    auto pd = dir_ / "pack";
    if (!fs::is_directory(pd, ec)) return;
    std::vector<fs::path> idxs;
    for (auto& e : fs::directory_iterator(pd, ec))
        if (e.path().extension() == ".idx") idxs.push_back(e.path());
    std::sort(idxs.begin(), idxs.end());
    for (auto& idx : idxs) {
        auto pack = idx;
        pack.replace_extension(".pack");
        if (!fs::exists(pack)) continue;
        try {
            packs_.push_back(std::make_unique<Pack>(pack, idx));
        } catch (const std::exception&) {
            // skip unreadable packs
        }
    }
}

fs::path ObjectDB::loose_path(const ObjectId& id) const {
    auto h = id.hex();
    return dir_ / h.substr(0, 2) / h.substr(2);
}

std::optional<Object> ObjectDB::try_read(const ObjectId& id) const {
    if (auto raw = try_read_file(loose_path(id))) {
        Bytes data = zlib_inflate(*raw);
        size_t sp = data.find(' '), nul = data.find('\0');
        if (sp == Bytes::npos || nul == Bytes::npos) throw Error("corrupt loose object " + id.hex());
        Object o;
        o.type = type_from_name(std::string_view(data).substr(0, sp));
        o.data = data.substr(nul + 1);
        return o;
    }
    std::function<std::optional<Object>(const ObjectId&)> resolver = [this](const ObjectId& x) {
        return try_read(x);
    };
    {
        std::vector<Pack*> packs;
        {
            std::lock_guard lk(mu_);
            for (auto& p : packs_) packs.push_back(p.get());
        }
        for (auto* p : packs)
            if (auto off = p->find(id)) return p->read_at(*off, resolver);
    }
    for (auto& a : alt_dbs_)
        if (auto o = a->try_read(id)) return o;
    return std::nullopt;
}

Object ObjectDB::read(const ObjectId& id) const {
    auto o = try_read(id);
    if (!o) throw Error("object not found: " + id.hex());
    return std::move(*o);
}

Object ObjectDB::read_expect(const ObjectId& id, ObjType t) const {
    auto o = read(id);
    if (o.type != t)
        throw Error("object " + id.short_hex() + " is a " + type_name(o.type) + ", expected " + type_name(t));
    return o;
}

Tree ObjectDB::read_tree(const ObjectId& id) const {
    auto o = read(id);
    if (o.type == ObjType::Commit) return read_tree(Commit::parse(o.data).tree);
    if (o.type != ObjType::Tree) throw Error("not a tree: " + id.short_hex());
    return Tree::parse(o.data);
}

ObjectId ObjectDB::peel_to_commit(ObjectId id) const {
    for (int i = 0; i < 16; i++) {
        auto o = read(id);
        if (o.type == ObjType::Commit) return id;
        if (o.type != ObjType::Tag) throw Error("cannot peel " + id.short_hex() + " to a commit");
        id = Tag::parse(o.data).object;
    }
    throw Error("tag chain too deep");
}

bool ObjectDB::exists(const ObjectId& id) const {
    std::error_code ec;
    if (fs::exists(loose_path(id), ec)) return true;
    {
        std::lock_guard lk(mu_);
        for (auto& p : packs_) if (p->contains(id)) return true;
    }
    for (auto& a : alt_dbs_) if (a->exists(id)) return true;
    return false;
}

ObjectId ObjectDB::write(ObjType t, std::string_view data) {
    ObjectId id = hash_object(t, data);
    if (exists(id)) return id;
    Bytes raw = object_header(t, data.size());
    raw += data;
    auto p = loose_path(id);
    fs::create_directories(p.parent_path());
    write_file_atomic(p, zlib_deflate(raw));
    return id;
}

std::vector<ObjectId> ObjectDB::resolve_prefix(std::string_view hex) const {
    std::vector<ObjectId> out;
    if (hex.size() < 4) return out;
    std::string h = to_lower(hex);
    std::error_code ec;
    auto dir = dir_ / h.substr(0, 2);
    if (fs::is_directory(dir, ec)) {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            auto name = h.substr(0, 2) + e.path().filename().string();
            if (name.size() == 40 && name.compare(0, h.size(), h) == 0)
                if (auto id = ObjectId::try_hex(name)) out.push_back(*id);
        }
    }
    std::lock_guard lk(mu_);
    for (auto& p : packs_) p->match_prefix(h, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace git
