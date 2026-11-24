// SPDX-License-Identifier: MIT
// Git object model and object database (loose objects + pack files).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core.hpp"

namespace git {

enum class ObjType : int { None = 0, Commit = 1, Tree = 2, Blob = 3, Tag = 4, OfsDelta = 6, RefDelta = 7 };
const char* type_name(ObjType t);
ObjType type_from_name(std::string_view s);

struct Object {
    ObjType type = ObjType::None;
    Bytes data;
};

// ---------------------------------------------------------------- signatures
struct Signature {
    std::string name, email;
    int64_t time = 0;
    int tz = 0;  // minutes east of UTC
    static Signature parse(std::string_view s);
    std::string format() const;
    static Signature now(std::string name, std::string email);
};

struct Commit {
    ObjectId tree;
    std::vector<ObjectId> parents;
    Signature author, committer;
    std::vector<std::pair<std::string, std::string>> extra_headers;  // gpgsig, encoding...
    std::string message;

    static Commit parse(std::string_view data);
    Bytes serialize() const;
    std::string summary() const;  // first line
    std::string body() const;     // rest, trimmed
};

struct TreeEntry {
    uint32_t mode = 0;  // 040000, 0100644, 0100755, 0120000, 0160000
    std::string name;
    ObjectId id;
    bool is_tree() const { return mode == 040000; }
    bool is_submodule() const { return mode == 0160000; }
};

struct Tree {
    std::vector<TreeEntry> entries;
    static Tree parse(std::string_view data);
    Bytes serialize() const;  // sorts using git's tree ordering
    const TreeEntry* find(std::string_view name) const;
};

struct Tag {
    ObjectId object;
    ObjType type = ObjType::Commit;
    std::string name;
    Signature tagger;
    std::string message;
    static Tag parse(std::string_view data);
    Bytes serialize() const;
};

ObjectId hash_object(ObjType t, std::string_view data);
Bytes object_header(ObjType t, size_t size);

// ---------------------------------------------------------------- mapping
class MappedFile {
public:
    explicit MappedFile(const fs::path& p);
    ~MappedFile();
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;
    void* map_ = nullptr;
#endif
    Bytes fallback_;
};

// ---------------------------------------------------------------- packs
class Pack {
public:
    Pack(const fs::path& pack, const fs::path& idx);
    std::optional<uint64_t> find(const ObjectId& id) const;
    bool contains(const ObjectId& id) const { return find(id).has_value(); }
    // Reads the object at `offset`. `resolve_ref` looks up REF_DELTA bases
    // that live outside this pack.
    Object read_at(uint64_t offset, const std::function<std::optional<Object>(const ObjectId&)>& resolve_ref) const;
    size_t count() const { return count_; }
    ObjectId id_at(size_t i) const;
    std::vector<ObjectId> all_ids() const;
    // find ids starting with a hex prefix
    void match_prefix(std::string_view hex, std::vector<ObjectId>& out) const;

private:
    std::unique_ptr<MappedFile> pack_, idx_;
    size_t count_ = 0;
    const uint8_t* fanout_ = nullptr;
    const uint8_t* ids_ = nullptr;
    const uint8_t* offs32_ = nullptr;
    const uint8_t* offs64_ = nullptr;
    mutable std::mutex cache_mu_;
    mutable std::unordered_map<uint64_t, std::shared_ptr<Object>> cache_;
    mutable std::vector<uint64_t> cache_order_;
};

Bytes apply_delta(std::string_view base, std::string_view delta);

// Turn a received pack (possibly containing deltas) into a .pack + .idx pair
// in `pack_dir`. Returns the pack checksum id. `external` resolves REF_DELTA
// bases that are already in the repository (thin packs).
ObjectId index_pack(std::string_view pack_data, const fs::path& pack_dir,
                    const std::function<std::optional<Object>(const ObjectId&)>& external,
                    const std::function<void(size_t, size_t)>& progress = {});

// Build a (non-delta) pack containing the given objects.
Bytes build_pack(const std::vector<std::pair<ObjectId, Object>>& objects);

// ---------------------------------------------------------------- ODB
class ObjectDB {
public:
    explicit ObjectDB(fs::path objects_dir);
    std::optional<Object> try_read(const ObjectId& id) const;
    Object read(const ObjectId& id) const;
    Object read_expect(const ObjectId& id, ObjType t) const;
    bool exists(const ObjectId& id) const;
    ObjectId write(ObjType t, std::string_view data);
    void reload_packs();
    std::vector<ObjectId> resolve_prefix(std::string_view hex) const;
    const fs::path& dir() const { return dir_; }

    Commit read_commit(const ObjectId& id) const { return Commit::parse(read_expect(id, ObjType::Commit).data); }
    Tree read_tree(const ObjectId& id) const;
    // peel tags until a commit
    ObjectId peel_to_commit(ObjectId id) const;

private:
    fs::path loose_path(const ObjectId& id) const;
    fs::path dir_;
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<Pack>> packs_;
    std::vector<fs::path> alternates_;
    std::vector<std::unique_ptr<ObjectDB>> alt_dbs_;
};

}  // namespace git
