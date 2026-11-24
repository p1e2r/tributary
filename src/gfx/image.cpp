// SPDX-License-Identifier: MIT
#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "../git/core.hpp"  // zlib_inflate, crc32

namespace gfx {

namespace {
[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
}  // namespace

uint64_t hash_bytes(std::string_view d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : d) { h ^= c; h *= 0x100000001b3ull; }
    return h ^ d.size();
}

bool is_image_data(std::string_view d) {
    return (d.size() > 8 && std::memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0) ||
           (d.size() > 3 && uint8_t(d[0]) == 0xFF && uint8_t(d[1]) == 0xD8) ||
           (d.size() > 6 && (std::memcmp(d.data(), "GIF87a", 6) == 0 || std::memcmp(d.data(), "GIF89a", 6) == 0));
}

// ================================================================ PNG
static Image decode_png(const uint8_t* d, size_t n) {
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    std::string idat;
    std::vector<uint8_t> palette, trns;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t* type = d + pos + 4;
        const uint8_t* data = d + pos + 8;
        if (pos + 12 + len > n) fail("truncated PNG");
        if (!std::memcmp(type, "IHDR", 4)) {
            w = be32(data); h = be32(data + 4); depth = data[8]; ctype = data[9]; interlace = data[12];
        } else if (!std::memcmp(type, "PLTE", 4)) palette.assign(data, data + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(data, data + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.append(reinterpret_cast<const char*>(data), len);
        else if (!std::memcmp(type, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (!w || !h || w > 16384 || h > 16384) fail("unsupported PNG size");
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels) fail("unsupported PNG color type");
    std::string raw = git::zlib_inflate(reinterpret_cast<const uint8_t*>(idat.data()), idat.size());
    int bpp_bits = channels * depth;
    int bpp = std::max(1, bpp_bits / 8);
    Image img;
    img.w = int(w);
    img.h = int(h);
    img.format = "PNG";
    img.rgba.assign(size_t(w) * h * 4, 255);
    auto sample = [&](const uint8_t* row, uint32_t x, int c) -> uint32_t {
        if (depth == 8) return row[x * uint32_t(channels) + uint32_t(c)];
        if (depth == 16) return be16(row + (x * uint32_t(channels) + uint32_t(c)) * 2);
        uint32_t bit = x * uint32_t(depth);
        return (row[bit / 8] >> (8 - depth - int(bit % 8))) & ((1u << depth) - 1);
    };
    auto to8 = [&](uint32_t v) -> uint8_t {
        if (depth == 16) return uint8_t(v >> 8);
        if (depth == 8) return uint8_t(v);
        return uint8_t(v * 255 / ((1u << depth) - 1));
    };
    size_t rp = 0;
    auto unfilter_pass = [&](uint32_t pw, uint32_t ph, auto&& put) {
        if (!pw || !ph) return;
        size_t stride = (size_t(pw) * size_t(bpp_bits) + 7) / 8;
        std::vector<uint8_t> prev(stride, 0), cur(stride);
        for (uint32_t y = 0; y < ph; y++) {
            if (rp + 1 + stride > raw.size()) fail("truncated PNG data");
            uint8_t ft = uint8_t(raw[rp++]);
            std::memcpy(cur.data(), raw.data() + rp, stride);
            rp += stride;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= size_t(bpp) ? cur[i - size_t(bpp)] : 0, b = prev[i], c = i >= size_t(bpp) ? prev[i - size_t(bpp)] : 0;
                int v = cur[i];
                switch (ft) {
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) / 2; break;
                    case 4: {
                        int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                        break;
                    }
                    default: break;
                }
                cur[i] = uint8_t(v);
            }
            for (uint32_t x = 0; x < pw; x++) put(x, y, cur.data());
            std::swap(prev, cur);
        }
    };
    auto store = [&](uint32_t px, uint32_t py, const uint8_t* row, uint32_t x) {
        uint8_t* o = &img.rgba[(size_t(py) * w + px) * 4];
        switch (ctype) {
            case 0: {
                uint32_t g = sample(row, x, 0);
                o[0] = o[1] = o[2] = to8(g);
                if (trns.size() >= 2 && g == be16(trns.data())) o[3] = 0;
                break;
            }
            case 2: {
                uint32_t r = sample(row, x, 0), g = sample(row, x, 1), b = sample(row, x, 2);
                o[0] = to8(r); o[1] = to8(g); o[2] = to8(b);
                if (trns.size() >= 6 && r == be16(trns.data()) && g == be16(trns.data() + 2) && b == be16(trns.data() + 4)) o[3] = 0;
                break;
            }
            case 3: {
                uint32_t i = sample(row, x, 0);
                if (i * 3 + 2 < palette.size()) { o[0] = palette[i * 3]; o[1] = palette[i * 3 + 1]; o[2] = palette[i * 3 + 2]; }
                o[3] = i < trns.size() ? trns[i] : 255;
                break;
            }
            case 4: o[0] = o[1] = o[2] = to8(sample(row, x, 0)); o[3] = to8(sample(row, x, 1)); break;
            case 6:
                o[0] = to8(sample(row, x, 0)); o[1] = to8(sample(row, x, 1)); o[2] = to8(sample(row, x, 2)); o[3] = to8(sample(row, x, 3));
                break;
        }
    };
    if (!interlace) {
        unfilter_pass(w, h, [&](uint32_t x, uint32_t y, const uint8_t* row) { store(x, y, row, x); });
    } else {
        static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; p++) {
            uint32_t pw = (w - uint32_t(sx[p]) + uint32_t(dx[p]) - 1) / uint32_t(dx[p]);
            uint32_t ph = (h - uint32_t(sy[p]) + uint32_t(dy[p]) - 1) / uint32_t(dy[p]);
            if (w <= uint32_t(sx[p])) pw = 0;
            if (h <= uint32_t(sy[p])) ph = 0;
            unfilter_pass(pw, ph, [&](uint32_t x, uint32_t y, const uint8_t* row) {
                store(uint32_t(sx[p]) + x * uint32_t(dx[p]), uint32_t(sy[p]) + y * uint32_t(dy[p]), row, x);
            });
        }
    }
    return img;
}

// ================================================================ JPEG (baseline)
namespace {
struct JHuff {
    uint8_t lookup_len[65536];
    uint8_t lookup_val[65536];
    bool present = false;
    void build(const uint8_t counts[16], const uint8_t* vals) {
        std::memset(lookup_len, 0, sizeof lookup_len);
        int code = 0, k = 0;
        for (int len = 1; len <= 16; len++) {
            for (int i = 0; i < counts[len - 1]; i++, k++) {
                int shift = 16 - len;
                for (int fill = 0; fill < (1 << shift); fill++) {
                    int idx = (code << shift) | fill;
                    lookup_len[idx] = uint8_t(len);
                    lookup_val[idx] = vals[k];
                }
                code++;
            }
            code <<= 1;
        }
        present = true;
    }
};

const int kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
                         41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                         30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JComp { int id, h, v, tq, td = 0, ta = 0, dc_pred = 0; std::vector<uint8_t> plane; int pw = 0, ph = 0; };

struct BitIn {
    const uint8_t* d; size_t n, pos;
    uint32_t bits = 0; int cnt = 0;
    bool marker_hit = false;
    void fill() {
        while (cnt <= 24) {
            uint32_t byte = 0;
            if (!marker_hit && pos < n) {
                byte = d[pos];
                if (byte == 0xFF) {
                    uint8_t nx = pos + 1 < n ? d[pos + 1] : 0;
                    if (nx == 0x00) pos += 2;
                    else { marker_hit = true; byte = 0; }
                } else pos++;
            }
            bits |= byte << (24 - cnt);
            cnt += 8;
        }
    }
    int decode(const JHuff& h) {
        fill();
        uint32_t peek = bits >> 16;
        int len = h.lookup_len[peek];
        if (!len) fail("corrupt JPEG Huffman data");
        bits <<= len;
        cnt -= len;
        return h.lookup_val[peek];
    }
    int receive(int s) {
        if (!s) return 0;
        fill();
        uint32_t v = bits >> (32 - s);
        bits <<= s;
        cnt -= s;
        if (v < (1u << (s - 1))) return int(v) - (1 << s) + 1;
        return int(v);
    }
    void reset() { bits = 0; cnt = 0; marker_hit = false; }
};

void idct8x8(const int in[64], uint8_t* out, int stride) {
    // straightforward float IDCT (fine for diff previews)
    static float c[8][8];
    static bool init = [] {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                c[x][u] = (u == 0 ? 1.0f / std::sqrt(2.0f) : 1.0f) * std::cos((2.f * float(x) + 1.f) * float(u) * 3.14159265f / 16.f);
        return true;
    }();
    (void)init;
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += c[y][v] * float(in[v * 8 + u]);
            tmp[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += c[x][u] * tmp[y * 8 + u];
            int v = int(std::lround(s / 4.f + 128.f));
            out[y * stride + x] = uint8_t(std::clamp(v, 0, 255));
        }
}
}  // namespace

static Image decode_jpeg(const uint8_t* d, size_t n) {
    uint16_t qt[4][64]{};
    auto dc = std::make_unique<JHuff[]>(4);
    auto ac = std::make_unique<JHuff[]>(4);
    std::vector<JComp> comps;
    int W = 0, H = 0, restart = 0, hmax = 1, vmax = 1, mcux = 0, mcuy = 0;
    bool progressive = false, have_frame = false;
    // coefficients per component, per block (bw*bh blocks of 64)
    std::vector<std::vector<int16_t>> coefs;
    std::vector<int> bw, bh;
    int eobrun = 0;
    size_t pos = 2;
    auto finish = [&]() {
        for (size_t ci = 0; ci < comps.size(); ci++) {
            auto& c = comps[ci];
            c.pw = mcux * c.h * 8;
            c.ph = mcuy * c.v * 8;
            c.plane.assign(size_t(c.pw) * size_t(c.ph), 0);
            for (int by = 0; by < mcuy * c.v; by++)
                for (int bx = 0; bx < mcux * c.h; bx++) {
                    int blk[64];
                    const int16_t* src = &coefs[ci][(size_t(by) * size_t(bw[ci]) + size_t(bx)) * 64];
                    for (int k = 0; k < 64; k++) blk[k] = int(src[k]) * qt[c.tq][k];
                    idct8x8(blk, &c.plane[size_t(by * 8) * size_t(c.pw) + size_t(bx * 8)], c.pw);
                }
        }
        Image img;
        img.w = W;
        img.h = H;
        img.format = "JPEG";
        img.rgba.assign(size_t(W) * size_t(H) * 4, 255);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                auto at = [&](const JComp& c) {
                    int sx = x * c.h / hmax, sy = y * c.v / vmax;
                    return float(c.plane[size_t(sy) * size_t(c.pw) + size_t(sx)]);
                };
                uint8_t* o = &img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
                if (comps.size() == 1) o[0] = o[1] = o[2] = uint8_t(at(comps[0]));
                else {
                    float Y = at(comps[0]), cb = at(comps[1]) - 128, cr = at(comps[2]) - 128;
                    o[0] = uint8_t(std::clamp(int(Y + 1.402f * cr), 0, 255));
                    o[1] = uint8_t(std::clamp(int(Y - 0.344136f * cb - 0.714136f * cr), 0, 255));
                    o[2] = uint8_t(std::clamp(int(Y + 1.772f * cb), 0, 255));
                }
            }
        return img;
    };
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) { pos++; continue; }
        uint8_t m = d[pos + 1];
        pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) continue;
        if (m == 0xD9) break;
        uint16_t len = be16(d + pos);
        const uint8_t* seg = d + pos + 2;
        size_t seg_len = len - 2u;
        if (pos + len > n) fail("truncated JPEG");
        switch (m) {
            case 0xDB: {
                size_t p = 0;
                while (p < seg_len) {
                    int pq = seg[p] >> 4, tq = seg[p] & 15;
                    p++;
                    for (int i = 0; i < 64; i++) qt[tq & 3][kZigzag[i]] = pq ? be16(seg + p + 2 * size_t(i)) : seg[p + size_t(i)];
                    p += pq ? 128 : 64;
                }
                break;
            }
            case 0xC4: {
                size_t p = 0;
                while (p < seg_len) {
                    int tc = seg[p] >> 4, th = seg[p] & 15;
                    const uint8_t* counts = seg + p + 1;
                    int total = 0;
                    for (int i = 0; i < 16; i++) total += counts[i];
                    (tc ? ac : dc)[th & 3].build(counts, seg + p + 17);
                    p += 17 + size_t(total);
                }
                break;
            }
            case 0xDD: restart = be16(seg); break;
            case 0xC0: case 0xC1: case 0xC2: {
                progressive = m == 0xC2;
                H = be16(seg + 1);
                W = be16(seg + 3);
                if (!W || !H) fail("JPEG with unknown size");
                int nc = seg[5];
                for (int i = 0; i < nc; i++) {
                    JComp c;
                    c.id = seg[6 + i * 3];
                    c.h = std::max(1, seg[7 + i * 3] >> 4);
                    c.v = std::max(1, seg[7 + i * 3] & 15);
                    c.tq = seg[8 + i * 3] & 3;
                    hmax = std::max(hmax, c.h);
                    vmax = std::max(vmax, c.v);
                    comps.push_back(c);
                }
                mcux = (W + 8 * hmax - 1) / (8 * hmax);
                mcuy = (H + 8 * vmax - 1) / (8 * vmax);
                for (auto& c : comps) {
                    bw.push_back(mcux * c.h);
                    bh.push_back(mcuy * c.v);
                    coefs.emplace_back(size_t(mcux * c.h) * size_t(mcuy * c.v) * 64, int16_t(0));
                }
                have_frame = true;
                break;
            }
            case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                fail("lossless/arithmetic JPEGs can't be previewed yet");
            case 0xDA: {
                if (!have_frame) fail("JPEG scan before frame");
                int ns = seg[0];
                std::vector<int> scomp;
                for (int i = 0; i < ns; i++) {
                    int cid = seg[1 + i * 2];
                    for (size_t k = 0; k < comps.size(); k++)
                        if (comps[k].id == cid) {
                            comps[k].td = seg[2 + i * 2] >> 4;
                            comps[k].ta = seg[2 + i * 2] & 15;
                            scomp.push_back(int(k));
                        }
                }
                int Ss = seg[1 + ns * 2], Se = seg[2 + ns * 2], Ah = seg[3 + ns * 2] >> 4, Al = seg[3 + ns * 2] & 15;
                if (!progressive) { Ss = 0; Se = 63; Ah = Al = 0; }
                BitIn bi{d, n, pos + len};
                for (auto& c : comps) c.dc_pred = 0;
                eobrun = 0;
                auto decode_block = [&](size_t ci, int16_t* co) {
                    auto& c = comps[ci];
                    if (!progressive) {
                        int t = bi.decode(dc[c.td & 3]);
                        c.dc_pred += bi.receive(t);
                        co[0] = int16_t(c.dc_pred);
                        for (int k = 1; k < 64;) {
                            int rs = bi.decode(ac[c.ta & 3]);
                            int r = rs >> 4, sz = rs & 15;
                            if (!sz) { if (r == 15) { k += 16; continue; } break; }
                            k += r;
                            if (k > 63) break;
                            co[kZigzag[k]] = int16_t(bi.receive(sz));
                            k++;
                        }
                        return;
                    }
                    if (Ss == 0) {  // DC scan
                        if (Ah == 0) {
                            int t = bi.decode(dc[c.td & 3]);
                            c.dc_pred += bi.receive(t);
                            co[0] = int16_t(c.dc_pred * (1 << Al));
                        } else {
                            // refinement: one raw bit
                            bi.fill();
                            uint32_t bit = bi.bits >> 31;
                            bi.bits <<= 1;
                            bi.cnt -= 1;
                            if (bit) co[0] = int16_t(co[0] | (1 << Al));
                        }
                        return;
                    }
                    if (Ah == 0) {  // AC first
                        if (eobrun) { eobrun--; return; }
                        for (int k = Ss; k <= Se;) {
                            int rs = bi.decode(ac[c.ta & 3]);
                            int r = rs >> 4, sz = rs & 15;
                            if (!sz) {
                                if (r < 15) {
                                    eobrun = (1 << r) - 1;
                                    if (r) { bi.fill(); eobrun += int(bi.bits >> (32 - r)); bi.bits <<= r; bi.cnt -= r; }
                                    break;
                                }
                                k += 16;
                                continue;
                            }
                            k += r;
                            if (k > 63) break;
                            co[kZigzag[k]] = int16_t(bi.receive(sz) * (1 << Al));
                            k++;
                        }
                        return;
                    }
                    // AC refinement
                    int p1 = 1 << Al, m1 = -1 * (1 << Al);
                    auto read_bit = [&]() { bi.fill(); uint32_t b = bi.bits >> 31; bi.bits <<= 1; bi.cnt -= 1; return int(b); };
                    int k = Ss;
                    if (!eobrun) {
                        for (; k <= Se;) {
                            int rs = bi.decode(ac[c.ta & 3]);
                            int r = rs >> 4, sz = rs & 15;
                            int val = 0;
                            if (!sz) {
                                if (r < 15) {
                                    eobrun = 1 << r;
                                    if (r) { bi.fill(); eobrun += int(bi.bits >> (32 - r)); bi.bits <<= r; bi.cnt -= r; }
                                    break;
                                }
                            } else {
                                val = read_bit() ? p1 : m1;
                            }
                            while (k <= Se) {
                                int16_t& z = co[kZigzag[k]];
                                if (z) {
                                    if (read_bit() && (z & p1) == 0) z = int16_t(z >= 0 ? z + p1 : z + m1);
                                } else {
                                    if (r == 0) {
                                        if (val) z = int16_t(val);
                                        k++;
                                        break;
                                    }
                                    r--;
                                }
                                k++;
                            }
                        }
                    }
                    if (eobrun) {
                        for (; k <= Se; k++) {
                            int16_t& z = co[kZigzag[k]];
                            if (z && read_bit() && (z & p1) == 0) z = int16_t(z >= 0 ? z + p1 : z + m1);
                        }
                        eobrun--;
                    }
                };
                auto handle_restart = [&](int count) {
                    if (restart && count && count % restart == 0) {
                        size_t p = bi.pos;
                        while (p + 1 < n && !(d[p] == 0xFF && d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7)) p++;
                        bi.pos = p + 2;
                        bi.reset();
                        for (auto& c : comps) c.dc_pred = 0;
                        eobrun = 0;
                    }
                };
                if (scomp.size() == 1) {
                    // non-interleaved: blocks cover only the component's real extent
                    size_t ci = size_t(scomp[0]);
                    auto& c = comps[ci];
                    int cw = ((W * c.h + hmax - 1) / hmax + 7) / 8, chh = ((H * c.v + vmax - 1) / vmax + 7) / 8;
                    int count = 0;
                    for (int by = 0; by < chh; by++)
                        for (int bx = 0; bx < cw; bx++) {
                            handle_restart(count++);
                            decode_block(ci, &coefs[ci][(size_t(by) * size_t(bw[ci]) + size_t(bx)) * 64]);
                        }
                } else {
                    int count = 0;
                    for (int my = 0; my < mcuy; my++)
                        for (int mx = 0; mx < mcux; mx++) {
                            handle_restart(count++);
                            for (int cii : scomp) {
                                size_t ci = size_t(cii);
                                auto& c = comps[ci];
                                for (int by = 0; by < c.v; by++)
                                    for (int bx = 0; bx < c.h; bx++) {
                                        int gx = mx * c.h + bx, gy = my * c.v + by;
                                        decode_block(ci, &coefs[ci][(size_t(gy) * size_t(bw[ci]) + size_t(gx)) * 64]);
                                    }
                            }
                        }
                }
                // continue after the entropy-coded segment
                size_t p = bi.pos;
                while (p + 1 < n && !(d[p] == 0xFF && d[p + 1] != 0 && !(d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7))) p++;
                pos = p;
                if (!progressive) return finish();
                continue;
            }
            default: break;
        }
        pos += len;
    }
    if (have_frame && progressive) return finish();
    fail("JPEG has no image data");
}

// ================================================================ GIF
static Image decode_gif(const uint8_t* d, size_t n) {
    if (n < 13) fail("truncated GIF");
    int W = d[6] | d[7] << 8, H = d[8] | d[9] << 8;
    uint8_t flags = d[10];
    size_t pos = 13;
    std::vector<uint8_t> gpal;
    if (flags & 0x80) {
        size_t sz = 3u << ((flags & 7) + 1);
        gpal.assign(d + pos, d + std::min(n, pos + sz));
        pos += sz;
    }
    int transparent = -1;
    Image img;
    img.w = W;
    img.h = H;
    img.format = "GIF";
    img.rgba.assign(size_t(W) * size_t(H) * 4, 0);
    while (pos < n) {
        uint8_t b = d[pos++];
        if (b == 0x21) {  // extension
            uint8_t label = d[pos++];
            if (label == 0xF9 && pos + 5 < n) {
                if (d[pos + 1] & 1) transparent = d[pos + 4];
            }
            while (pos < n && d[pos]) pos += d[pos] + 1u;
            pos++;
        } else if (b == 0x2C) {  // image descriptor
            int ix = d[pos] | d[pos + 1] << 8, iy = d[pos + 2] | d[pos + 3] << 8;
            int iw = d[pos + 4] | d[pos + 5] << 8, ih = d[pos + 6] | d[pos + 7] << 8;
            uint8_t lf = d[pos + 8];
            pos += 9;
            std::vector<uint8_t> pal = gpal;
            if (lf & 0x80) {
                size_t sz = 3u << ((lf & 7) + 1);
                pal.assign(d + pos, d + std::min(n, pos + sz));
                pos += sz;
            }
            bool interlaced = lf & 0x40;
            int min_code = d[pos++];
            std::string data;
            while (pos < n && d[pos]) {
                data.append(reinterpret_cast<const char*>(d + pos + 1), d[pos]);
                pos += d[pos] + 1u;
            }
            pos++;
            // LZW
            std::vector<uint8_t> out;
            out.reserve(size_t(iw) * size_t(ih));
            int clear = 1 << min_code, eoi = clear + 1;
            std::vector<std::vector<uint8_t>> table;
            auto reset = [&] {
                table.clear();
                for (int i = 0; i < clear; i++) table.push_back({uint8_t(i)});
                table.push_back({});
                table.push_back({});
            };
            reset();
            int size = min_code + 1;
            uint32_t acc = 0;
            int bits = 0;
            int prev = -1;
            for (unsigned char ch : data) {
                acc |= uint32_t(ch) << bits;
                bits += 8;
                while (bits >= size) {
                    int code = int(acc & ((1u << size) - 1));
                    acc >>= size;
                    bits -= size;
                    if (code == clear) { reset(); size = min_code + 1; prev = -1; continue; }
                    if (code == eoi) { bits = 0; break; }
                    std::vector<uint8_t> entry;
                    if (code < int(table.size())) entry = table[size_t(code)];
                    else if (prev >= 0) { entry = table[size_t(prev)]; entry.push_back(table[size_t(prev)][0]); }
                    else fail("corrupt GIF data");
                    out.insert(out.end(), entry.begin(), entry.end());
                    if (prev >= 0 && table.size() < 4096) {
                        auto ne = table[size_t(prev)];
                        ne.push_back(entry[0]);
                        table.push_back(ne);
                        if (int(table.size()) == (1 << size) && size < 12) size++;
                    }
                    prev = code;
                }
            }
            auto row_of = [&](int r) {
                if (!interlaced) return r;
                int passes[4][2] = {{0, 8}, {4, 8}, {2, 4}, {1, 2}};
                int idx = r;
                for (auto& p : passes) {
                    int cnt = (ih - p[0] + p[1] - 1) / p[1];
                    if (idx < cnt) return p[0] + idx * p[1];
                    idx -= cnt;
                }
                return r;
            };
            for (int y = 0; y < ih; y++)
                for (int x = 0; x < iw; x++) {
                    size_t i = size_t(y) * size_t(iw) + size_t(x);
                    if (i >= out.size()) break;
                    int px = ix + x, py = iy + row_of(y);
                    if (px >= W || py >= H) continue;
                    uint8_t ci = out[i];
                    uint8_t* o = &img.rgba[(size_t(py) * size_t(W) + size_t(px)) * 4];
                    if (ci == transparent) continue;
                    if (size_t(ci) * 3 + 2 < pal.size()) { o[0] = pal[ci * 3u]; o[1] = pal[ci * 3u + 1]; o[2] = pal[ci * 3u + 2]; }
                    o[3] = 255;
                }
            return img;  // first frame only
        } else if (b == 0x3B) break;
    }
    fail("GIF has no frames");
}

Image decode_image(std::string_view data) {
    auto d = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size();
    if (n > 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return decode_png(d, n);
    if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) return decode_jpeg(d, n);
    if (n > 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return decode_gif(d, n);
    fail("unsupported image format");
}

Image difference_image(const Image& a, const Image& b) {
    Image o;
    o.w = std::max(a.w, b.w);
    o.h = std::max(a.h, b.h);
    o.format = "diff";
    o.rgba.assign(size_t(o.w) * size_t(o.h) * 4, 255);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            auto px = [&](const Image& im, int c) -> int {
                if (x >= im.w || y >= im.h) return 0;
                const uint8_t* p = &im.rgba[(size_t(y) * size_t(im.w) + size_t(x)) * 4];
                return p[c] * p[3] / 255;  // premultiply against black
            };
            uint8_t* q = &o.rgba[(size_t(y) * size_t(o.w) + size_t(x)) * 4];
            for (int c = 0; c < 3; c++) q[c] = uint8_t(std::abs(px(a, c) - px(b, c)));
            q[3] = 255;
        }
    return o;
}

}  // namespace gfx
