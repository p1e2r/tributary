// SPDX-License-Identifier: MIT
// From-scratch DEFLATE (RFC 1951) decoder/encoder wrapped in zlib (RFC 1950).
#include <algorithm>

#include "core.hpp"

namespace git {

// ============================================================ inflate
namespace {

struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;
    uint64_t bits = 0;
    int count = 0;

    void need(int k) {
        while (count < k) {
            if (pos >= n) throw Error("zlib: unexpected end of stream");
            bits |= uint64_t(p[pos++]) << count;
            count += 8;
        }
    }
    uint32_t get(int k) {
        if (k == 0) return 0;
        need(k);
        uint32_t v = uint32_t(bits & ((1ull << k) - 1));
        bits >>= k;
        count -= k;
        return v;
    }
    void align() {
        int drop = count % 8;
        bits >>= drop;
        count -= drop;
    }
    // Bytes consumed so far, not counting whole bytes still buffered.
    size_t consumed() const { return pos - size_t(count / 8); }
};

// Canonical Huffman decoder using a fast 9-bit table + slow path.
struct Huffman {
    static constexpr int FAST = 9;
    uint16_t counts[16]{};
    std::vector<uint16_t> symbols;
    int16_t fast_sym[1 << FAST];
    uint8_t fast_len[1 << FAST];

    void build(const uint8_t* lengths, int n) {
        std::fill(std::begin(counts), std::end(counts), 0);
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        counts[0] = 0;
        uint16_t offs[16];
        offs[1] = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = uint16_t(offs[i] + counts[i]);
        symbols.assign(n, 0);
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbols[offs[lengths[i]]++] = uint16_t(i);
        // fast table
        std::fill(std::begin(fast_len), std::end(fast_len), 0);
        int code = 0, idx = 0;
        for (int len = 1; len <= FAST; len++) {
            for (int k = 0; k < counts[len]; k++, code++, idx++) {
                // reverse bits of code (len bits)
                int rev = 0;
                for (int b = 0; b < len; b++) if (code & (1 << b)) rev |= 1 << (len - 1 - b);
                for (int fill = rev; fill < (1 << FAST); fill += 1 << len) {
                    fast_sym[fill] = int16_t(symbols[idx]);
                    fast_len[fill] = uint8_t(len);
                }
            }
            code <<= 1;
        }
    }

    int decode(BitReader& br) const {
        // try fast path
        if (br.count < FAST) {
            while (br.count < FAST && br.pos < br.n) {
                br.bits |= uint64_t(br.p[br.pos++]) << br.count;
                br.count += 8;
            }
        }
        if (br.count >= 1) {
            uint32_t peek = uint32_t(br.bits & ((1u << FAST) - 1));
            int l = fast_len[peek];
            if (l && l <= br.count) {
                br.bits >>= l;
                br.count -= l;
                return fast_sym[peek];
            }
        }
        // slow canonical decode
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= int(br.get(1));
            int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Error("zlib: bad huffman code");
    }
};

const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                            6145, 8193, 12289, 16385, 24577};
const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

Bytes raw_inflate(const uint8_t* in, size_t in_len, size_t* consumed, size_t size_hint) {
    BitReader br{in, in_len};
    Bytes out;
    out.reserve(size_hint ? size_hint : in_len * 3);

    static Huffman fixed_lit, fixed_dist;
    static bool fixed_init = [] {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        fixed_lit.build(l, 288);
        uint8_t d[30];
        std::fill(d, d + 30, 5);
        fixed_dist.build(d, 30);
        return true;
    }();
    (void)fixed_init;

    Huffman dyn_lit, dyn_dist;
    bool last = false;
    while (!last) {
        last = br.get(1);
        uint32_t type = br.get(2);
        if (type == 0) {
            br.align();
            uint32_t len = br.get(16), nlen = br.get(16);
            if ((len ^ 0xFFFF) != nlen) throw Error("zlib: stored block length mismatch");
            // bits buffer is byte-aligned now; drain it first
            while (len && br.count >= 8) { out.push_back(char(br.get(8))); len--; }
            if (br.pos + len > br.n) throw Error("zlib: truncated stored block");
            out.append(reinterpret_cast<const char*>(br.p + br.pos), len);
            br.pos += len;
            continue;
        }
        const Huffman *lit, *dist;
        if (type == 1) {
            lit = &fixed_lit;
            dist = &fixed_dist;
        } else if (type == 2) {
            int hlit = int(br.get(5)) + 257, hdist = int(br.get(5)) + 1, hclen = int(br.get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                              11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19]{};
            for (int i = 0; i < hclen; i++) cl[order[i]] = uint8_t(br.get(3));
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lens[320]{};
            int i = 0;
            while (i < hlit + hdist) {
                int sym = clh.decode(br);
                if (sym < 16) lens[i++] = uint8_t(sym);
                else if (sym == 16) {
                    if (i == 0) throw Error("zlib: repeat with no previous length");
                    int r = 3 + int(br.get(2));
                    uint8_t prev = lens[i - 1];
                    while (r-- && i < 320) lens[i++] = prev;
                } else if (sym == 17) {
                    int r = 3 + int(br.get(3));
                    while (r-- && i < 320) lens[i++] = 0;
                } else {
                    int r = 11 + int(br.get(7));
                    while (r-- && i < 320) lens[i++] = 0;
                }
            }
            dyn_lit.build(lens, hlit);
            dyn_dist.build(lens + hlit, hdist);
            lit = &dyn_lit;
            dist = &dyn_dist;
        } else {
            throw Error("zlib: invalid block type");
        }
        for (;;) {
            int sym = lit->decode(br);
            if (sym < 256) {
                out.push_back(char(sym));
            } else if (sym == 256) {
                break;
            } else {
                sym -= 257;
                if (sym >= 29) throw Error("zlib: bad length symbol");
                size_t len = LBASE[sym] + br.get(LEXT[sym]);
                int ds = dist->decode(br);
                if (ds >= 30) throw Error("zlib: bad distance symbol");
                size_t d = DBASE[ds] + br.get(DEXT[ds]);
                if (d > out.size()) throw Error("zlib: distance too far back");
                size_t from = out.size() - d;
                if (d >= len) {
                    out.append(out, from, len);
                } else {
                    for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
                }
            }
        }
    }
    br.align();
    size_t used = br.consumed();
    if (consumed) *consumed = used;
    return out;
}

Bytes zlib_inflate(const uint8_t* in, size_t in_len, size_t* consumed, size_t size_hint) {
    if (in_len < 2) throw Error("zlib: stream too short");
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0) throw Error("zlib: bad header");
    if (flg & 0x20) throw Error("zlib: preset dictionary unsupported");
    size_t used = 0;
    Bytes out = raw_inflate(in + 2, in_len - 2, &used, size_hint);
    // Adler-32 trailer
    if (2 + used + 4 > in_len) throw Error("zlib: missing checksum");
    const uint8_t* t = in + 2 + used;
    uint32_t want = uint32_t(t[0]) << 24 | uint32_t(t[1]) << 16 | uint32_t(t[2]) << 8 | t[3];
    if (adler32(out.data(), out.size()) != want) throw Error("zlib: checksum mismatch");
    if (consumed) *consumed = 2 + used + 4;
    return out;
}

// ============================================================ deflate
namespace {

struct BitWriter {
    Bytes out;
    uint64_t bits = 0;
    int count = 0;
    void put(uint32_t v, int n) {
        bits |= uint64_t(v) << count;
        count += n;
        while (count >= 8) {
            out.push_back(char(bits & 0xFF));
            bits >>= 8;
            count -= 8;
        }
    }
    // Huffman codes are sent MSB-first, so reverse.
    void put_code(uint32_t code, int len) {
        uint32_t rev = 0;
        for (int i = 0; i < len; i++) if (code & (1u << i)) rev |= 1u << (len - 1 - i);
        put(rev, len);
    }
    void flush() {
        if (count) { out.push_back(char(bits & 0xFF)); bits = 0; count = 0; }
    }
};

struct Token {
    uint16_t len;   // 0 => literal
    uint16_t dist;  // or literal byte
};

// Build length-limited canonical Huffman code lengths (package-merge-lite:
// simple heuristic — build Huffman, then flatten overly long codes).
void build_lengths(const std::vector<uint32_t>& freq, int max_len, std::vector<uint8_t>& lens) {
    int n = int(freq.size());
    lens.assign(n, 0);
    struct Node { uint64_t w; int l, r, sym; };
    std::vector<Node> nodes;
    std::vector<int> heap;
    auto cmp = [&](int a, int b) { return nodes[a].w > nodes[b].w; };
    for (int i = 0; i < n; i++)
        if (freq[i]) { nodes.push_back({freq[i], -1, -1, i}); heap.push_back(int(nodes.size()) - 1); }
    if (heap.empty()) return;
    if (heap.size() == 1) { lens[nodes[heap[0]].sym] = 1; return; }
    std::make_heap(heap.begin(), heap.end(), cmp);
    while (heap.size() > 1) {
        std::pop_heap(heap.begin(), heap.end(), cmp); int a = heap.back(); heap.pop_back();
        std::pop_heap(heap.begin(), heap.end(), cmp); int b = heap.back(); heap.pop_back();
        nodes.push_back({nodes[a].w + nodes[b].w, a, b, -1});
        heap.push_back(int(nodes.size()) - 1);
        std::push_heap(heap.begin(), heap.end(), cmp);
    }
    // depth assignment
    std::vector<std::pair<int, int>> st{{heap[0], 0}};
    while (!st.empty()) {
        auto [i, d] = st.back(); st.pop_back();
        if (nodes[i].sym >= 0) lens[nodes[i].sym] = uint8_t(std::max(1, d));
        else { st.push_back({nodes[i].l, d + 1}); st.push_back({nodes[i].r, d + 1}); }
    }
    // limit lengths: Kraft-sum fixup
    bool over = false;
    for (auto l : lens) if (l > max_len) over = true;
    if (!over) return;
    for (auto& l : lens) if (l > max_len) l = uint8_t(max_len);
    auto kraft = [&] {
        uint64_t s = 0;
        for (auto l : lens) if (l) s += 1ull << (max_len - l);
        return s;
    };
    uint64_t cap = 1ull << max_len;
    while (kraft() > cap) {
        // lengthen the longest code that is shorter than max_len
        int best = -1;
        for (int i = 0; i < n; i++)
            if (lens[i] && lens[i] < max_len && (best < 0 || lens[i] > lens[best])) best = i;
        if (best < 0) break;
        lens[best]++;
    }
    // make the code complete again (zlib rejects incomplete multi-code sets)
    for (;;) {
        uint64_t k = kraft();
        if (k >= cap) break;
        int best = -1;
        for (int i = 0; i < n; i++)
            if (lens[i] > 1 && k + (1ull << (max_len - lens[i])) <= cap &&
                (best < 0 || lens[i] > lens[best]))
                best = i;
        if (best < 0) break;
        lens[best]--;
    }
}

void canonical_codes(const std::vector<uint8_t>& lens, std::vector<uint16_t>& codes) {
    int n = int(lens.size());
    codes.assign(n, 0);
    uint16_t bl_count[16]{}, next[16]{};
    for (auto l : lens) bl_count[l]++;
    bl_count[0] = 0;
    uint16_t code = 0;
    for (int b = 1; b < 16; b++) { code = uint16_t((code + bl_count[b - 1]) << 1); next[b] = code; }
    for (int i = 0; i < n; i++) if (lens[i]) codes[i] = next[lens[i]]++;
}

int len_symbol(int len, int& extra_bits, int& extra) {
    extra_bits = 0;
    extra = 0;
    for (int i = 28; i >= 0; i--)
        if (len >= LBASE[i]) { extra_bits = LEXT[i]; extra = len - LBASE[i]; return 257 + i; }
    return 257;
}
int dist_symbol(int d, int& extra_bits, int& extra) {
    extra_bits = 0;
    extra = 0;
    for (int i = 29; i >= 0; i--)
        if (d >= DBASE[i]) { extra_bits = DEXT[i]; extra = d - DBASE[i]; return i; }
    return 0;
}

void emit_block(BitWriter& bw, const std::vector<Token>& toks, bool last) {
    std::vector<uint32_t> lf(286, 0), df(30, 0);
    for (auto& t : toks) {
        if (!t.len) lf[t.dist]++;
        else {
            int eb, e;
            lf[len_symbol(t.len, eb, e)]++;
            df[dist_symbol(t.dist, eb, e)]++;
        }
    }
    lf[256] = 1;
    std::vector<uint8_t> ll, dl;
    build_lengths(lf, 15, ll);
    build_lengths(df, 15, dl);
    bool anyd = false;
    for (auto l : dl) if (l) anyd = true;
    if (!anyd) dl[0] = 1;
    int hlit = 286; while (hlit > 257 && !ll[hlit - 1]) hlit--;
    int hdist = 30; while (hdist > 1 && !dl[hdist - 1]) hdist--;

    // run-length encode code lengths
    std::vector<uint8_t> all(ll.begin(), ll.begin() + hlit);
    all.insert(all.end(), dl.begin(), dl.begin() + hdist);
    struct CL { uint8_t sym, extra, ebits; };
    std::vector<CL> cls;
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j < all.size() && all[j] == all[i]) j++;
        size_t run = j - i;
        if (all[i] == 0) {
            while (run >= 11) { size_t r = std::min<size_t>(run, 138); cls.push_back({18, uint8_t(r - 11), 7}); run -= r; }
            if (run >= 3) { cls.push_back({17, uint8_t(run - 3), 3}); run = 0; }
            while (run--) cls.push_back({0, 0, 0});
        } else {
            cls.push_back({all[i], 0, 0}); run--;
            while (run >= 3) { size_t r = std::min<size_t>(run, 6); cls.push_back({16, uint8_t(r - 3), 2}); run -= r; }
            while (run--) cls.push_back({all[i], 0, 0});
        }
        i = j;
    }
    std::vector<uint32_t> cf(19, 0);
    for (auto& c : cls) cf[c.sym]++;
    {  // the code-length code must be complete: guarantee two symbols
        int used = 0;
        for (auto f : cf) if (f) used++;
        if (used < 2) { if (!cf[0]) cf[0] = 1; else cf[1] = 1; }
    }
    std::vector<uint8_t> cll;
    build_lengths(cf, 7, cll);
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hclen = 19; while (hclen > 4 && !cll[order[hclen - 1]]) hclen--;
    std::vector<uint16_t> lc, dc, cc;
    canonical_codes(ll, lc);
    canonical_codes(dl, dc);
    canonical_codes(cll, cc);

    bw.put(last ? 1 : 0, 1);
    bw.put(2, 2);
    bw.put(uint32_t(hlit - 257), 5);
    bw.put(uint32_t(hdist - 1), 5);
    bw.put(uint32_t(hclen - 4), 4);
    for (int i = 0; i < hclen; i++) bw.put(cll[order[i]], 3);
    for (auto& c : cls) {
        bw.put_code(cc[c.sym], cll[c.sym]);
        if (c.ebits) bw.put(c.extra, c.ebits);
    }
    for (auto& t : toks) {
        if (!t.len) bw.put_code(lc[t.dist], ll[t.dist]);
        else {
            int eb, e;
            int s = len_symbol(t.len, eb, e);
            bw.put_code(lc[s], ll[s]);
            if (eb) bw.put(uint32_t(e), eb);
            int ds = dist_symbol(t.dist, eb, e);
            bw.put_code(dc[ds], dl[ds]);
            if (eb) bw.put(uint32_t(e), eb);
        }
    }
    bw.put_code(lc[256], ll[256]);
}

}  // namespace

Bytes zlib_deflate(std::string_view in, int level) {
    BitWriter bw;
    bw.out.push_back(char(0x78));
    bw.out.push_back(char(0x9C));
    const uint8_t* p = reinterpret_cast<const uint8_t*>(in.data());
    size_t n = in.size();
    const int WIN = 32768, HBITS = 15, HSIZE = 1 << HBITS;
    int max_chain = level <= 1 ? 4 : level <= 6 ? 32 : 128;
    std::vector<int32_t> head(HSIZE, -1), prev(n ? n : 1, -1);
    auto hash = [&](size_t i) {
        return ((uint32_t(p[i]) << 10) ^ (uint32_t(p[i + 1]) << 5) ^ p[i + 2]) & (HSIZE - 1);
    };
    std::vector<Token> toks;
    toks.reserve(65536);
    size_t i = 0;
    auto flush_block = [&](bool last) {
        emit_block(bw, toks, last);
        toks.clear();
    };
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = hash(i);
            int32_t cand = head[h];
            int chain = max_chain;
            size_t maxl = std::min<size_t>(258, n - i);
            while (cand >= 0 && chain-- && i - size_t(cand) <= size_t(WIN)) {
                const uint8_t* a = p + cand;
                const uint8_t* b = p + i;
                if (a[best_len] == b[best_len]) {
                    size_t l = 0;
                    while (l < maxl && a[l] == b[l]) l++;
                    if (int(l) > best_len) {
                        best_len = int(l);
                        best_dist = int(i - size_t(cand));
                        if (l == maxl) break;
                    }
                }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = int32_t(i);
        }
        if (best_len >= 3) {
            toks.push_back({uint16_t(best_len), uint16_t(best_dist)});
            // insert hashes for skipped positions
            for (size_t k = i + 1; k < i + size_t(best_len) && k + 3 <= n; k++) {
                uint32_t h = hash(k);
                prev[k] = head[h];
                head[h] = int32_t(k);
            }
            i += size_t(best_len);
        } else {
            toks.push_back({0, p[i]});
            i++;
        }
        if (toks.size() >= 60000) flush_block(false);
    }
    flush_block(true);
    bw.flush();
    uint32_t a = adler32(p, n);
    bw.out.push_back(char(a >> 24));
    bw.out.push_back(char(a >> 16));
    bw.out.push_back(char(a >> 8));
    bw.out.push_back(char(a));
    return std::move(bw.out);
}

}  // namespace git
