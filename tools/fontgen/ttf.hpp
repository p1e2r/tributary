// SPDX-License-Identifier: MIT
// Minimal TrueType writer: glyf/loca outlines (straight segments), cmap format 4,
// hmtx, OS/2 v4, name, post v3, optional GPOS pair kerning.
#pragma once
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "geom.hpp"

namespace fg {

struct Glyph {
    uint32_t cp = 0;  // 0 = .notdef
    double advance = 500;
    std::vector<Contour> contours;
};

struct FontInfo {
    std::string family, style, full, postscript, version = "Version 1.000";
    int upem = 1000, ascender = 969, descender = -242, line_gap = 0;
    int cap_height = 727, x_height = 546;
    int weight = 400;
    bool bold = false, mono = false;
    std::map<std::pair<uint32_t, uint32_t>, int> kerning;  // codepoint pairs
};

class Buf {
public:
    std::vector<uint8_t> d;
    void u8(uint32_t v) { d.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void i16(int v) { u16(uint16_t(int16_t(v))); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void tag(const char* t) { for (int i = 0; i < 4; i++) u8(uint8_t(t[i])); }
    void bytes(const std::vector<uint8_t>& b) { d.insert(d.end(), b.begin(), b.end()); }
    void pad4() { while (d.size() % 4) u8(0); }
    size_t size() const { return d.size(); }
    void put16(size_t at, uint32_t v) { d[at] = uint8_t(v >> 8); d[at + 1] = uint8_t(v); }
    void put32(size_t at, uint32_t v) { put16(at, v >> 16); put16(at + 2, v & 0xFFFF); }
};

inline uint32_t checksum(const std::vector<uint8_t>& t) {
    uint32_t s = 0;
    for (size_t i = 0; i < t.size(); i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) v = v << 8 | (i + k < t.size() ? t[i + k] : 0);
        s += v;
    }
    return s;
}

inline std::vector<uint8_t> write_ttf(const FontInfo& fi, std::vector<Glyph> glyphs) {
    // glyph 0 is .notdef; the rest sorted by code point
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.cp < b.cp; });
    size_t ng = glyphs.size();
    struct GData { std::vector<uint8_t> bytes; int xmin = 0, ymin = 0, xmax = 0, ymax = 0, pts = 0, nc = 0; };
    std::vector<GData> gd(ng);
    int gxmin = 30000, gymin = 30000, gxmax = -30000, gymax = -30000, maxpts = 0, maxc = 0;
    for (size_t i = 0; i < ng; i++) {
        auto& g = glyphs[i];
        std::vector<std::vector<std::pair<int, int>>> cs;
        for (auto& c : g.contours) {
            std::vector<std::pair<int, int>> q;
            for (auto& p : c) {
                std::pair<int, int> v{int(std::lround(p.x)), int(std::lround(p.y))};
                if (q.empty() || q.back() != v) q.push_back(v);
            }
            while (q.size() > 1 && q.front() == q.back()) q.pop_back();
            if (q.size() >= 3) cs.push_back(q);
        }
        auto& G = gd[i];
        if (cs.empty()) continue;
        G.xmin = G.ymin = 30000;
        G.xmax = G.ymax = -30000;
        for (auto& c : cs)
            for (auto [x, y] : c) {
                G.xmin = std::min(G.xmin, x); G.xmax = std::max(G.xmax, x);
                G.ymin = std::min(G.ymin, y); G.ymax = std::max(G.ymax, y);
                G.pts++;
            }
        G.nc = int(cs.size());
        Buf b;
        b.i16(G.nc);
        b.i16(G.xmin); b.i16(G.ymin); b.i16(G.xmax); b.i16(G.ymax);
        int end = -1;
        for (auto& c : cs) { end += int(c.size()); b.u16(uint32_t(end)); }
        b.u16(0);  // no instructions
        std::vector<uint8_t> flags;
        Buf xs, ys;
        int px = 0, py = 0;
        for (auto& c : cs)
            for (auto [x, y] : c) {
                int dx = x - px, dy = y - py;
                uint8_t f = 1;  // on curve
                if (dx == 0) f |= 0x10;
                else if (std::abs(dx) < 256) { f |= 0x02; if (dx > 0) f |= 0x10; xs.u8(uint32_t(std::abs(dx))); }
                else xs.i16(dx);
                if (dy == 0) f |= 0x20;
                else if (std::abs(dy) < 256) { f |= 0x04; if (dy > 0) f |= 0x20; ys.u8(uint32_t(std::abs(dy))); }
                else ys.i16(dy);
                flags.push_back(f);
                px = x;
                py = y;
            }
        b.bytes(flags);
        b.bytes(xs.d);
        b.bytes(ys.d);
        while (b.size() % 2) b.u8(0);
        G.bytes = b.d;
        gxmin = std::min(gxmin, G.xmin); gymin = std::min(gymin, G.ymin);
        gxmax = std::max(gxmax, G.xmax); gymax = std::max(gymax, G.ymax);
        maxpts = std::max(maxpts, G.pts);
        maxc = std::max(maxc, G.nc);
    }
    std::map<std::string, std::vector<uint8_t>> T;
    // glyf + loca
    {
        Buf glyf, loca;
        for (size_t i = 0; i < ng; i++) {
            loca.u32(uint32_t(glyf.size()));
            glyf.bytes(gd[i].bytes);
            while (glyf.size() % 4) glyf.u8(0);
        }
        loca.u32(uint32_t(glyf.size()));
        T["glyf"] = glyf.d;
        T["loca"] = loca.d;
    }
    int adv_max = 0, min_lsb = 30000, min_rsb = 30000, xmax_ext = 0;
    double avg = 0;
    int navg = 0;
    for (size_t i = 0; i < ng; i++) {
        int adv = int(std::lround(glyphs[i].advance));
        adv_max = std::max(adv_max, adv);
        if (gd[i].nc) {
            min_lsb = std::min(min_lsb, gd[i].xmin);
            min_rsb = std::min(min_rsb, adv - gd[i].xmax);
            xmax_ext = std::max(xmax_ext, gd[i].xmax);
        }
        if (glyphs[i].cp >= 32 && glyphs[i].cp < 127) { avg += adv; navg++; }
    }
    {
        Buf h;
        h.u32(0x00010000); h.u32(0x00010000);
        h.u32(0);  // checkSumAdjustment, patched later
        h.u32(0x5F0F3CF5);
        h.u16(0x000B);
        h.u16(uint32_t(fi.upem));
        h.u32(0); h.u32(0xE3A8C000);  // created (2024)
        h.u32(0); h.u32(0xE3A8C000);  // modified
        h.i16(gxmin); h.i16(gymin); h.i16(gxmax); h.i16(gymax);
        h.u16(fi.bold ? 1 : 0);
        h.u16(8);
        h.i16(2);
        h.i16(1);  // long loca
        h.i16(0);
        T["head"] = h.d;
    }
    {
        Buf h;
        h.u32(0x00010000);
        h.i16(fi.ascender); h.i16(fi.descender); h.i16(fi.line_gap);
        h.u16(uint32_t(adv_max)); h.i16(min_lsb); h.i16(min_rsb); h.i16(xmax_ext);
        h.i16(1); h.i16(0); h.i16(0);
        for (int i = 0; i < 4; i++) h.i16(0);
        h.i16(0);
        h.u16(uint32_t(ng));
        T["hhea"] = h.d;
    }
    {
        Buf m;
        m.u32(0x00010000);
        m.u16(uint32_t(ng));
        m.u16(uint32_t(maxpts)); m.u16(uint32_t(maxc));
        m.u16(0); m.u16(0);
        m.u16(2);
        m.u16(0); m.u16(0); m.u16(0); m.u16(0); m.u16(0); m.u16(0); m.u16(0); m.u16(0);
        T["maxp"] = m.d;
    }
    {
        Buf h;
        for (size_t i = 0; i < ng; i++) {
            h.u16(uint32_t(std::lround(glyphs[i].advance)));
            h.i16(gd[i].nc ? gd[i].xmin : 0);
        }
        T["hmtx"] = h.d;
    }
    {
        // cmap format 4: one segment per run of consecutive code points
        struct Seg { uint32_t start, end; int delta; };
        std::vector<Seg> segs;
        for (size_t i = 0; i < ng; i++) {
            uint32_t cp = glyphs[i].cp;
            if (cp == 0 || cp > 0xFFFE) continue;
            int delta = int(i) - int(cp);
            if (!segs.empty() && segs.back().end + 1 == cp && segs.back().delta == delta) segs.back().end = cp;
            else segs.push_back({cp, cp, delta});
        }
        segs.push_back({0xFFFF, 0xFFFF, 1});
        uint32_t n = uint32_t(segs.size());
        uint32_t sr = 1, es = 0;
        while (sr * 2 <= n) { sr *= 2; es++; }
        Buf f;
        f.u16(4);
        f.u16(16 + 8 * n);
        f.u16(0);
        f.u16(n * 2); f.u16(sr * 2); f.u16(es); f.u16(n * 2 - sr * 2);
        for (auto& s : segs) f.u16(s.end);
        f.u16(0);
        for (auto& s : segs) f.u16(s.start);
        for (auto& s : segs) f.u16(uint32_t(uint16_t(int16_t(s.delta))));
        for (size_t i = 0; i < segs.size(); i++) f.u16(0);
        Buf c;
        c.u16(0); c.u16(1);
        c.u16(3); c.u16(1); c.u32(12);
        c.bytes(f.d);
        T["cmap"] = c.d;
    }
    {
        Buf o;
        o.u16(4);
        o.i16(int(navg ? avg / navg : 500));
        o.u16(uint32_t(fi.weight));
        o.u16(5);
        o.u16(0);  // installable
        o.i16(650); o.i16(600); o.i16(0); o.i16(75);   // subscript
        o.i16(650); o.i16(600); o.i16(0); o.i16(350);  // superscript
        o.i16(50); o.i16(280);                         // strikeout
        o.i16(8 << 8);                                 // sans serif
        // panose
        uint8_t pan[10] = {2, 11, uint8_t(fi.bold ? 7 : 5), uint8_t(fi.mono ? 9 : 2), 2, 2, 2, 2, 2, 4};
        for (auto v : pan) o.u8(v);
        o.u32(0x00000003); o.u32(0); o.u32(0); o.u32(0);  // Basic Latin, Latin-1
        o.tag("TRIB");
        o.u16(fi.bold ? 0x00A0 : 0x00C0);  // BOLD|USE_TYPO or REGULAR|USE_TYPO
        o.u16(32); o.u16(0xFFFF);
        o.i16(fi.ascender); o.i16(fi.descender); o.i16(fi.line_gap);
        o.u16(uint32_t(std::max(fi.ascender, gymax))); o.u16(uint32_t(std::max(-fi.descender, -gymin)));
        o.u32(1); o.u32(0);
        o.i16(fi.x_height); o.i16(fi.cap_height);
        o.u16(0); o.u16(32); o.u16(2);
        T["OS/2"] = o.d;
    }
    {
        Buf p;
        p.u32(0x00030000);
        p.u32(0);
        p.i16(-100); p.i16(60);
        p.u32(fi.mono ? 1 : 0);
        p.u32(0); p.u32(0); p.u32(0); p.u32(0);
        T["post"] = p.d;
    }
    {
        std::vector<std::pair<int, std::string>> names = {
            {0, "Copyright (c) Tributary contributors. MIT License."}, {1, fi.family}, {2, fi.style},
            {3, fi.postscript + ";" + fi.version}, {4, fi.full}, {5, fi.version}, {6, fi.postscript},
            {13, "MIT License"}};
        Buf strs, n;
        n.u16(0); n.u16(uint32_t(names.size())); n.u16(uint32_t(6 + 12 * names.size()));
        for (auto& [id, s] : names) {
            size_t off = strs.size();
            for (unsigned char ch : s) strs.u16(ch);  // ASCII → UTF-16BE
            n.u16(3); n.u16(1); n.u16(0x409); n.u16(uint32_t(id)); n.u16(uint32_t(strs.size() - off)); n.u16(uint32_t(off));
        }
        n.bytes(strs.d);
        T["name"] = n.d;
    }
    if (!fi.kerning.empty()) {
        // GPOS: one PairPos format 1 lookup under the 'kern' feature (DFLT and latn)
        std::map<uint32_t, uint16_t> gid;
        for (size_t i = 0; i < ng; i++) gid[glyphs[i].cp] = uint16_t(i);
        std::map<uint16_t, std::vector<std::pair<uint16_t, int>>> firsts;
        for (auto& [pr, v] : fi.kerning)
            if (gid.count(pr.first) && gid.count(pr.second) && pr.first) firsts[gid[pr.first]].push_back({gid[pr.second], v});
        Buf pp;  // PairPosFormat1
        size_t nfirst = firsts.size();
        pp.u16(1);
        size_t cov_off_at = pp.size(); pp.u16(0);
        pp.u16(0x0004);  // XAdvance
        pp.u16(0);
        pp.u16(uint32_t(nfirst));
        std::vector<size_t> set_off_at;
        for (size_t i = 0; i < nfirst; i++) { set_off_at.push_back(pp.size()); pp.u16(0); }
        size_t k = 0;
        for (auto& [g1, list] : firsts) {
            std::sort(list.begin(), list.end());
            pp.put16(set_off_at[k++], uint32_t(pp.size()));
            pp.u16(uint32_t(list.size()));
            for (auto& [g2, v] : list) { pp.u16(g2); pp.i16(v); }
        }
        pp.put16(cov_off_at, uint32_t(pp.size()));
        pp.u16(1); pp.u16(uint32_t(nfirst));
        for (auto& [g1, list] : firsts) pp.u16(g1);
        Buf lk;  // Lookup
        lk.u16(2); lk.u16(0); lk.u16(1); lk.u16(8);
        lk.bytes(pp.d);
        Buf ll;
        ll.u16(1); ll.u16(4);
        ll.bytes(lk.d);
        Buf fl;  // FeatureList: 1 feature 'kern' -> lookup 0
        fl.u16(1); fl.tag("kern"); fl.u16(8);
        fl.u16(0); fl.u16(1); fl.u16(0);
        Buf sl;  // ScriptList: DFLT + latn, default langsys with feature 0
        sl.u16(2);
        sl.tag("DFLT"); sl.u16(14);
        sl.tag("latn"); sl.u16(14 + 12);
        for (int s = 0; s < 2; s++) { sl.u16(4); sl.u16(0); sl.u16(0); sl.u16(0xFFFF); sl.u16(1); sl.u16(0); }
        Buf g;
        g.u32(0x00010000);
        g.u16(10); g.u16(uint32_t(10 + sl.size())); g.u16(uint32_t(10 + sl.size() + fl.size()));
        g.bytes(sl.d); g.bytes(fl.d); g.bytes(ll.d);
        T["GPOS"] = g.d;
    }
    // assemble
    uint16_t nt = uint16_t(T.size());
    uint16_t sr = 1, es = 0;
    while (sr * 2 <= nt) { sr *= 2; es++; }
    Buf f;
    f.u32(0x00010000);
    f.u16(nt); f.u16(sr * 16); f.u16(es); f.u16(nt * 16 - sr * 16);
    size_t dir_at = f.size();
    for (size_t i = 0; i < nt; i++) { f.u32(0); f.u32(0); f.u32(0); f.u32(0); }
    size_t head_at = 0, i = 0;
    for (auto& [tag, data] : T) {  // std::map keeps tags sorted, as required
        size_t off = f.size();
        if (tag == "head") head_at = off;
        f.bytes(data);
        f.pad4();
        size_t e = dir_at + 16 * i++;
        for (int k = 0; k < 4; k++) f.d[e + k] = uint8_t(tag[k]);
        f.put32(e + 4, checksum(data));
        f.put32(e + 8, uint32_t(off));
        f.put32(e + 12, uint32_t(data.size()));
    }
    f.put32(head_at + 8, 0xB1B0AFBA - checksum(f.d));
    return f.d;
}

}  // namespace fg
