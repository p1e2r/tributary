// SPDX-License-Identifier: MIT
// Tributary's font generator. Every glyph is designed here as a centreline skeleton
// (lines, Bézier curves and superellipse arcs) and stroked with an elliptical pen, so one
// set of drawings yields Tributary Sans Regular, Tributary Sans Bold and Tributary Mono.
//
//   fontgen <out-dir>                 writes the three .ttf files
//   fontgen --specimen <out.pgm> ...  renders sample text for checking the designs
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "geom.hpp"
#include "ttf.hpp"

using namespace fg;

namespace {

enum Mark { M_grave, M_acute, M_circumflex, M_tilde, M_macron, M_breve, M_dotabove, M_dieresis, M_ring, M_hungarumlaut,
            M_caron, M_cedilla, M_ogonek, M_commabelow, M_horn };
struct Decomp { uint32_t cp, base; std::vector<Mark> marks; };
const Decomp kDecomp[] = {
#include "decomp.inc"
};
struct BoxDef { uint32_t cp; int u, d, l, r, dash; };
const BoxDef kBox[] = {
#include "boxdraw.inc"
};

struct Style {
    const char* name;
    double hx, hy;  // half stroke: vertical stems, horizontal bars
    bool mono;
    int weight;
};

// ---------------------------------------------------------------- the designer
struct D {
    Style st;
    Pen pen;
    double hx, hy, ex;  // ex: extra width heavier weights get
    // vertical metrics (units per em = 1000)
    static constexpr double CAP = 727, XH = 546, ASC = 770, DSC = -218, O = 11;
    static constexpr double LINE_TOP = 969, LINE_BOT = -242;  // terminal cell
    Glyph* g = nullptr;
    // horizontal transform applied to skeletons (monospace fitting)
    double mx = 1, bx = 0;

    explicit D(Style s) : st(s), pen{s.hx, s.hy}, hx(s.hx), hy(s.hy), ex((s.hx - 43) * 2.2) {}

    Pt T(Pt p) const { return {p.x * mx + bx, p.y}; }
    Path P() const { Path p; return p; }
    void s(Path p, Clip c = {}, double ml = 4) {
        for (auto& q : p.p) q = T(q);
        if (c.x0 > -1e8) c.x0 = c.x0 * mx + bx;
        if (c.x1 < 1e8) c.x1 = c.x1 * mx + bx;
        stroke(p, pen, g->contours, c, ml);
    }
    void s(Path p, Pen custom, Clip c = {}) {
        for (auto& q : p.p) q = T(q);
        if (c.x0 > -1e8) c.x0 = c.x0 * mx + bx;
        if (c.x1 < 1e8) c.x1 = c.x1 * mx + bx;
        stroke(p, custom, g->contours, c);
    }
    void stem(double x, double y0, double y1, Clip c = {}) { s(P().M(x, y0).L(x, y1), c); }
    void bar(double x0, double x1, double y, Clip c = {}) { s(P().M(x0, y).L(x1, y), c); }
    void line(double x0, double y0, double x1, double y1, Clip c = {}) { s(P().M(x0, y0).L(x1, y1), c); }
    // a closed bowl with these *outer* bounds
    void bowl(double l, double r, double b, double t, double sn = 2.25) {
        double cx = (l + r) / 2, cy = (b + t) / 2;
        Path p;
        p.A(cx, cy, (r - l) / 2 - hx, (t - b) / 2 - hy, 0, 360, sn).Z();
        s(p);
    }
    void dot(double cx, double cy, double scale = 1) {
        double r = std::max(hx, hy) * 1.18 * scale;
        Pt c = T({cx, cy});
        g->contours.push_back(superellipse(c.x, c.y, r * 1.0, r * 1.0, 2.4, 40));
    }
    void fill(Contour c) {
        for (auto& p : c) p = T(p);
        make_cw(c);
        g->contours.push_back(c);
    }
    double dot_r() const { return std::max(hx, hy) * 1.18; }
    // n-style arch from a left stem at x0 to a right stem at x1, top of skeleton `top`
    void arch(double x0, double x1, double top, double yend = 0, double shift = 4) {
        double rx = (x1 - x0) / 2, xm = x0 + rx + shift;
        double ry = std::min((x1 - xm) * 1.02, top * 0.62);
        double yj = top - ry * 1.32;
        Path p;
        p.M(x0, yj).C(x0, yj + (top - yj) * 0.55, xm - (xm - x0) * 0.58, top, xm, top).A(xm, top - ry, x1 - xm, ry, 90, 0).L(x1, yend);
        s(p);
    }
};

using Design = std::function<double(D&)>;  // draws into d.g, returns advance

// x on a line through (xa,ya)-(xb,yb) at height y
double xat(double xa, double ya, double xb, double yb, double y) { return xa + (xb - xa) * (y - ya) / (yb - ya); }

// ---------------------------------------------------------------- glyph designs
std::map<uint32_t, Design> designs() {
    std::map<uint32_t, Design> m;
    const double CAP = D::CAP, XH = D::XH, ASC = D::ASC, DSC = D::DSC, O = D::O;

    m[' '] = [](D&) { return 262.0; };
    m[0xA0] = m[' '];

    // ---------------------------------------------------------------- lowercase
    m['n'] = [=](D& d) {
        double L = 64, R = L + 438 + d.ex, x0 = L + d.hx, x1 = R - d.hx;
        d.stem(x0, 0, XH);
        d.arch(x0, x1, XH + O - d.hy);
        return R + 64;
    };
    m['h'] = [=](D& d) {
        double L = 64, R = L + 438 + d.ex, x0 = L + d.hx, x1 = R - d.hx;
        d.stem(x0, 0, ASC);
        d.arch(x0, x1, XH + O - d.hy);
        return R + 64;
    };
    m['m'] = [=](D& d) {
        double L = 64, R = L + 718 + d.ex * 1.8, x0 = L + d.hx, x2 = R - d.hx, x1 = (x0 + x2) / 2;
        d.stem(x0, 0, XH);
        d.arch(x0, x1, XH + O - d.hy, 0, 2);
        d.arch(x1, x2, XH + O - d.hy, 0, 2);
        return R + 64;
    };
    m['u'] = [=](D& d) {
        double L = 64, R = L + 438 + d.ex, x0 = L + d.hx, x1 = R - d.hx, adv = R + 64;
        // the n turned around
        double save_mx = d.mx, save_bx = d.bx;
        Glyph tmp;
        Glyph* real = d.g;
        d.g = &tmp;
        d.mx = 1;
        d.bx = 0;
        d.stem(x0, 0, XH);
        d.arch(x0, x1, XH + O - d.hy);
        d.g = real;
        d.mx = save_mx;
        d.bx = save_bx;
        for (auto& c : tmp.contours) {
            Contour q;
            for (auto& p : c) q.push_back(d.T({adv - p.x, XH - p.y}));
            make_cw(q);
            d.g->contours.push_back(q);
        }
        return adv;
    };
    m['r'] = [=](D& d) {
        double L = 64, x0 = L + d.hx, top = XH + O - d.hy;
        d.stem(x0, 0, XH);
        double ry = 120, yj = top - ry * 1.32, xe = x0 + 168 + d.ex * 0.5;
        Path p;
        p.M(x0, yj).C(x0, yj + (top - yj) * 0.6, x0 + 52, top, x0 + 118, top).L(xe, top - 2);
        d.s(p);
        return xe + d.hx * 0.3 + 26;
    };
    m['o'] = [=](D& d) {
        double L = 44, R = L + 470 + d.ex;
        d.bowl(L, R, -O, XH + O);
        return R + 44;
    };
    m['c'] = [=](D& d) {
        double L = 44, R = L + 436 + d.ex;
        double cx = (L + R) / 2, cy = XH / 2, rx = (R - L) / 2 - d.hx, ry = XH / 2 + O - d.hy;
        Path p;
        p.A(cx, cy, rx, ry, 36, 324);
        d.s(p);
        return R + 36;
    };
    m['e'] = [=](D& d) {
        double L = 44, R = L + 462 + d.ex;
        double cx = (L + R) / 2, cy = XH / 2 + 4, rx = (R - L) / 2 - d.hx, ry = XH / 2 + O - d.hy;
        Path p;
        p.M(cx - rx, cy - 6).L(cx + rx, cy - 6).A(cx, cy, rx, ry, -2, 322);
        d.s(p);
        return R + 40;
    };
    m['a'] = [=](D& d) {
        double L = 46, R = L + 418 + d.ex, x0 = L + d.hx, x1 = R - d.hx, top = XH + O - d.hy, bot = d.hy - O;
        double xm = (x0 + x1) / 2 + 6;
        Path p;
        p.M(x1, 0).L(x1, XH * 0.60).C(x1, top - 20, x1 - (x1 - xm) * 0.4, top, xm, top).C(xm - (xm - x0) * 0.62, top, x0 + 6, top - 52, x0 + 8, XH * 0.76);
        d.s(p);
        double yb = XH * 0.555, cxb = (x0 + x1) / 2 - 12, rxb = cxb - x0, ryb = (yb - bot) / 2, cyb = bot + ryb;
        Path b;
        b.M(x1, yb).L(cxb, yb).A(cxb, cyb, rxb, ryb, 90, 270).C(cxb + rxb * 0.62, bot, x1 - 6, bot + 40, x1, XH * 0.25);
        d.s(b);
        return R + 64;
    };
    auto bowl_d = [=](D& d, double L, double R, bool left_stem, double y0, double y1) {
        // single-storey bowl + stem; left_stem mirrors it (b, p)
        double sx = left_stem ? L + d.hx : R - d.hx;
        double ox = left_stem ? R - d.hx : L + d.hx;  // far side
        double top = XH + O - d.hy, bot = d.hy - O, cy = (top + bot) / 2, ry = (top - bot) / 2;
        double cx = (sx + ox) / 2, rx = std::fabs(cx - ox);
        double sg = left_stem ? -1 : 1;  // direction from bowl centre to stem
        Path p;
        p.M(sx, XH * 0.79).C(sx - sg * rx * 0.36, top, cx + sg * rx * 0.42, top, cx, top);
        if (left_stem) p.A(cx, cy, rx, ry, 90, -90);
        else p.A(cx, cy, rx, ry, 90, 270);
        p.C(cx + sg * rx * 0.42, bot, sx - sg * rx * 0.36, bot, sx, XH * 0.21);
        d.s(p);
        d.stem(sx, y0, y1);
    };
    m['d'] = [=](D& d) { double L = 44, R = L + 472 + d.ex; bowl_d(d, L, R, false, 0, ASC); return R + 62; };
    m['q'] = [=](D& d) { double L = 44, R = L + 472 + d.ex; bowl_d(d, L, R, false, DSC, XH); return R + 62; };
    m['b'] = [=](D& d) { double L = 62, R = L + 472 + d.ex; bowl_d(d, L, R, true, 0, ASC); return R + 44; };
    m['p'] = [=](D& d) { double L = 62, R = L + 472 + d.ex; bowl_d(d, L, R, true, DSC, XH); return R + 44; };
    m['g'] = [=](D& d) {
        double L = 44, R = L + 470 + d.ex, x0 = L + d.hx, x1 = R - d.hx;
        // bowl like q, then a descender hook
        double top = XH + O - d.hy, bot = d.hy - O + 6, cy = (top + bot) / 2, ry = (top - bot) / 2, cx = (x0 + x1) / 2, rx = cx - x0;
        Path p;
        p.M(x1, XH * 0.79).C(x1 - rx * 0.36, top, cx + rx * 0.42, top, cx, top).A(cx, cy, rx, ry, 90, 270)
            .C(cx + rx * 0.42, bot, x1 - rx * 0.36, bot, x1, XH * 0.22);
        d.s(p);
        double bt = DSC + d.hy - O;
        Path t;
        t.M(x1, XH).L(x1, -18).C(x1, bt + 70, cx + rx * 0.62, bt, cx, bt).C(cx - rx * 0.6, bt, x0 + 8, bt + 34, x0 + 4, bt + 86);
        d.s(t);
        return R + 62;
    };
    m['s'] = [=](D& d) {
        double L = 40, R = L + 412 + d.ex, x0 = L + d.hx, x1 = R - d.hx, top = XH + O - d.hy, bot = d.hy - O;
        double ym = XH * 0.515, xm = (x0 + x1) / 2;
        double rxu = (x1 - x0) / 2 - 10, ryu = (top - ym) / 2, cyu = ym + ryu;
        double rxl = (x1 - x0) / 2, ryl = (ym - bot) / 2, cyl = bot + ryl;
        Path p;
        p.superness = 2.1;
        p.A(xm - 6, cyu, rxu, ryu, 22, 270).A(xm, cyl, rxl, ryl, 90, -158);
        d.s(p);
        return R + 40;
    };
    m['f'] = [=](D& d) {
        double L = 30, x0 = L + 64 + d.hx, topf = ASC + O - d.hy;
        Path p;
        p.M(x0, 0).L(x0, ASC - 150).C(x0, topf - 26, x0 + 36, topf, x0 + 104, topf).L(x0 + 168 + d.ex * 0.4, topf - 4);
        d.s(p);
        d.bar(L, x0 + 162 + d.ex * 0.4, XH - d.hy);
        return x0 + 168 + d.ex * 0.4 + 18;
    };
    m['t'] = [=](D& d) {
        double L = 26, x0 = L + 60 + d.hx, bot = d.hy - O;
        Path p;
        p.M(x0, 676).L(x0, 118).C(x0, bot + 26, x0 + 34, bot, x0 + 96, bot).L(x0 + 150 + d.ex * 0.4, bot + 4);
        d.s(p);
        d.bar(L, x0 + 158 + d.ex * 0.4, XH - d.hy);
        return x0 + 158 + d.ex * 0.4 + 20;
    };
    m['i'] = [=](D& d) {
        double x0 = 70 + d.hx;
        d.stem(x0, 0, XH);
        d.dot(x0, XH + 150 + d.dot_r());
        return x0 + d.hx + 70;
    };
    m[0x131] = [=](D& d) { double x0 = 70 + d.hx; d.stem(x0, 0, XH); return x0 + d.hx + 70; };  // dotless i
    m['j'] = [=](D& d) {
        double x0 = 70 + d.hx, bt = DSC + d.hy - O;
        Path p;
        p.M(x0, XH).L(x0, -30).C(x0, bt + 40, x0 - 30, bt, x0 - 86, bt).L(x0 - 118, bt + 2);
        d.s(p);
        d.dot(x0, XH + 150 + d.dot_r());
        return x0 + d.hx + 70;
    };
    m[0x237] = [=](D& d) {  // dotless j
        double x0 = 70 + d.hx, bt = DSC + d.hy - O;
        Path p;
        p.M(x0, XH).L(x0, -30).C(x0, bt + 40, x0 - 30, bt, x0 - 86, bt).L(x0 - 118, bt + 2);
        d.s(p);
        return x0 + d.hx + 70;
    };
    m['l'] = [=](D& d) { double x0 = 70 + d.hx; d.stem(x0, 0, ASC); return x0 + d.hx + 70; };
    m['k'] = [=](D& d) {
        double L = 64, R = L + 430 + d.ex, x0 = L + d.hx;
        d.stem(x0, 0, ASC);
        Clip c{-1e9, 0, R, XH};
        double ax0 = R - d.hx * 0.6, ay0 = XH, ax1 = x0 + d.hx * 0.3, ay1 = XH * 0.31;
        d.line(xat(ax0, ay0, ax1, ay1, XH + 60), XH + 60, ax1, ay1, c);
        double lx = x0 + (R - x0) * 0.30, ly = xat(ay1, ax1, ay0, ax0, lx);  // y on the arm at lx
        ly = ay1 + (ay0 - ay1) * (lx - ax1) / (ax0 - ax1);
        d.line(lx, ly, R + 30, -50, c);
        return R + 18;
    };
    auto vshape = [=](D& d, double L, double R, double h, double apex_y) {
        double xm = (L + R) / 2, tl = L + d.hx * 1.05, tr = R - d.hx * 1.05;
        Clip c{-1e9, 0, 1e9, h};
        d.s(d.P().M(xat(tl, h, xm, apex_y, h + 80), h + 80).L(xm, apex_y).L(xat(tr, h, xm, apex_y, h + 80), h + 80), c, 6);
    };
    m['v'] = [=](D& d) { double L = 20, R = L + 474 + d.ex; vshape(d, L, R, XH, 10); return R + 20; };
    m['w'] = [=](D& d) {
        double L = 22, R = L + 724 + d.ex * 1.6, w = R - L;
        double t0 = L + d.hx, q1 = L + w * 0.25, mid = L + w * 0.5, q3 = L + w * 0.75, t4 = R - d.hx;
        Clip c{-1e9, 0, 1e9, XH};
        Path p;
        p.M(xat(t0, XH, q1, 10, XH + 80), XH + 80).L(q1, 10).L(mid, XH + 6).L(q3, 10).L(xat(t4, XH, q3, 10, XH + 80), XH + 80);
        d.s(p, c, 6);
        return R + 22;
    };
    m['x'] = [=](D& d) {
        double L = 24, R = L + 452 + d.ex;
        Clip c{L - 20, 0, R + 20, XH};
        double a = L + d.hx, b = R - d.hx;
        d.line(xat(a, XH, b, 0, XH + 60), XH + 60, xat(a, XH, b, 0, -60), -60, c);
        d.line(xat(b, XH, a, 0, XH + 60), XH + 60, xat(b, XH, a, 0, -60), -60, c);
        return R + 24;
    };
    m['y'] = [=](D& d) {
        double L = 20, R = L + 474 + d.ex, xm = (L + R) / 2, tl = L + d.hx * 1.05, tr = R - d.hx * 1.05;
        d.line(xat(tr, XH, xm, 10, XH + 80), XH + 80, xat(tr, XH, xm, 10, DSC - 40), DSC - 40, Clip{-1e9, DSC, 1e9, XH});
        d.line(xat(tl, XH, xm, 10, XH + 80), XH + 80, xm, 10, Clip{-1e9, -1e9, 1e9, XH});
        return R + 20;
    };
    m['z'] = [=](D& d) {
        double L = 40, R = L + 418 + d.ex;
        Path p;
        p.M(L + 6, XH - d.hy).L(R - d.hx * 0.4, XH - d.hy).L(L + d.hx * 0.4, d.hy).L(R, d.hy);
        d.s(p, Clip{L, 0, R, XH}, 10);
        return R + 40;
    };

    // ---------------------------------------------------------------- uppercase
    m['H'] = [=](D& d) {
        double L = 78, R = L + 572 + d.ex, x0 = L + d.hx, x1 = R - d.hx;
        d.stem(x0, 0, CAP);
        d.stem(x1, 0, CAP);
        d.bar(x0, x1, CAP * 0.5);
        return R + 78;
    };
    m['I'] = [=](D& d) { double x0 = 84 + d.hx; d.stem(x0, 0, CAP); return x0 + d.hx + 84; };
    m['E'] = [=](D& d) {
        double L = 78, R = L + 440 + d.ex, x0 = L + d.hx;
        d.s(d.P().M(R, CAP - d.hy).L(x0, CAP - d.hy).L(x0, d.hy).L(R, d.hy));
        d.bar(x0, R - 24, CAP * 0.505);
        return R + 60;
    };
    m['F'] = [=](D& d) {
        double L = 78, R = L + 430 + d.ex, x0 = L + d.hx;
        d.s(d.P().M(R, CAP - d.hy).L(x0, CAP - d.hy).L(x0, 0));
        d.bar(x0, R - 30, CAP * 0.49);
        return R + 56;
    };
    m['L'] = [=](D& d) {
        double L = 78, R = L + 418 + d.ex, x0 = L + d.hx;
        d.s(d.P().M(x0, CAP).L(x0, d.hy).L(R, d.hy));
        return R + 50;
    };
    m['T'] = [=](D& d) {
        double L = 30, R = L + 566 + d.ex, xm = (L + R) / 2;
        d.bar(L, R, CAP - d.hy);
        d.stem(xm, 0, CAP - d.hy);
        return R + 30;
    };
    m['O'] = [=](D& d) { double L = 52, R = L + 680 + d.ex; d.bowl(L, R, -O, CAP + O, 2.2); return R + 52; };
    m['Q'] = [=](D& d) {
        double L = 52, R = L + 680 + d.ex;
        d.bowl(L, R, -O, CAP + O, 2.2);
        double xm = (L + R) / 2;
        d.line(xm + 108, 182, R + 6, -64, Clip{-1e9, -60, 1e9, 1e9});
        return R + 52;
    };
    m['C'] = [=](D& d) {
        double L = 52, R = L + 612 + d.ex, cx = (L + R) / 2, cy = CAP / 2, rx = (R - L) / 2 - d.hx, ry = CAP / 2 + O - d.hy;
        d.s(d.P().A(cx, cy, rx, ry, 38, 322, 2.2));
        return R + 44;
    };
    m['G'] = [=](D& d) {
        double L = 52, R = L + 640 + d.ex, cx = (L + R) / 2, cy = CAP / 2, rx = (R - L) / 2 - d.hx, ry = CAP / 2 + O - d.hy;
        Path p;
        p.A(cx, cy, rx, ry, 38, 352, 2.2);
        double xe = p.p.back().x, yb = CAP * 0.44;
        p.L(xe, yb).L(cx + 30, yb);
        d.s(p);
        return R + 56;
    };
    m['D'] = [=](D& d) {
        double L = 78, R = L + 600 + d.ex, x0 = L + d.hx, x1 = R - d.hx, xc = x0 + (x1 - x0) * 0.44;
        Path p;
        p.M(x0, d.hy).L(x0, CAP - d.hy).L(xc, CAP - d.hy).A(xc, CAP / 2, x1 - xc, CAP / 2 - d.hy, 90, -90, 2.3).L(x0, d.hy).Z();
        d.s(p);
        return R + 52;
    };
    m['B'] = [=](D& d) {
        double L = 78, R = L + 506 + d.ex, x0 = L + d.hx, x1 = R - d.hx, ymid = CAP * 0.535;
        double x1u = x1 - 34, cyu = (CAP - d.hy + ymid) / 2, ryu = (CAP - d.hy - ymid) / 2, xcu = x1u - ryu * 1.0;
        Path u;
        u.M(x0, ymid).L(x0, CAP - d.hy).L(xcu, CAP - d.hy).A(xcu, cyu, x1u - xcu, ryu, 90, -90, 2.3).L(x0, ymid);
        d.s(u);
        double cyl = (ymid + d.hy) / 2, ryl = (ymid - d.hy) / 2, xcl = x1 - ryl * 1.0;
        Path l;
        l.M(x0, ymid).L(xcl, ymid).A(xcl, cyl, x1 - xcl, ryl, 90, -90, 2.3).L(x0, d.hy).L(x0, ymid);
        d.s(l);
        return R + 54;
    };
    m['P'] = [=](D& d) {
        double L = 78, R = L + 486 + d.ex, x0 = L + d.hx, x1 = R - d.hx, ymid = CAP * 0.43;
        double cy = (CAP - d.hy + ymid) / 2, ry = (CAP - d.hy - ymid) / 2, xc = x1 - ry * 1.0;
        Path p;
        p.M(x0, 0).L(x0, CAP - d.hy).L(xc, CAP - d.hy).A(xc, cy, x1 - xc, ry, 90, -90, 2.3).L(x0, ymid);
        d.s(p);
        return R + 48;
    };
    m['R'] = [=](D& d) {
        double L = 78, R = L + 500 + d.ex, x0 = L + d.hx, x1 = R - d.hx - 6, ymid = CAP * 0.44;
        double cy = (CAP - d.hy + ymid) / 2, ry = (CAP - d.hy - ymid) / 2, xc = x1 - ry * 1.0;
        Path p;
        p.M(x0, 0).L(x0, CAP - d.hy).L(xc, CAP - d.hy).A(xc, cy, x1 - xc, ry, 90, -90, 2.3).L(x0, ymid);
        d.s(p);
        d.line(xc + 10, ymid, R + 20, -40, Clip{-1e9, 0, R, 1e9});
        return R + 50;
    };
    m['S'] = [=](D& d) {
        double L = 46, R = L + 528 + d.ex, x0 = L + d.hx, x1 = R - d.hx, top = CAP + O - d.hy, bot = d.hy - O;
        double ym = CAP * 0.52, xm = (x0 + x1) / 2;
        double rxu = (x1 - x0) / 2 - 12, ryu = (top - ym) / 2, cyu = ym + ryu;
        double rxl = (x1 - x0) / 2, ryl = (ym - bot) / 2, cyl = bot + ryl;
        Path p;
        p.superness = 2.15;
        p.A(xm - 8, cyu, rxu, ryu, 24, 270).A(xm, cyl, rxl, ryl, 90, -156);
        d.s(p);
        return R + 46;
    };
    m['A'] = [=](D& d) {
        double L = 18, R = L + 668 + d.ex, xm = (L + R) / 2, bl = L + d.hx * 1.05, br = R - d.hx * 1.05;
        Clip c{-1e9, 0, 1e9, CAP};
        double ay = CAP - 6;
        d.s(d.P().M(xat(bl, 0, xm, ay, -80), -80).L(xm, ay).L(xat(br, 0, xm, ay, -80), -80), c, 6);
        double yb = CAP * 0.27;
        d.bar(xat(bl, 0, xm, ay, yb), xat(br, 0, xm, ay, yb), yb);
        return R + 18;
    };
    m['V'] = [=](D& d) { double L = 18, R = L + 664 + d.ex; vshape(d, L, R, CAP, 8); return R + 18; };
    m['W'] = [=](D& d) {
        double L = 20, R = L + 940 + d.ex * 1.6, w = R - L;
        double t0 = L + d.hx, q1 = L + w * 0.235, mid = L + w * 0.5, q3 = L + w * 0.765, t4 = R - d.hx;
        Path p;
        p.M(xat(t0, CAP, q1, 8, CAP + 80), CAP + 80).L(q1, 8).L(mid, CAP + 6).L(q3, 8).L(xat(t4, CAP, q3, 8, CAP + 80), CAP + 80);
        d.s(p, Clip{-1e9, 0, 1e9, CAP}, 6);
        return R + 20;
    };
    m['X'] = [=](D& d) {
        double L = 24, R = L + 624 + d.ex, a = L + d.hx, b = R - d.hx;
        Clip c{L - 20, 0, R + 20, CAP};
        d.line(xat(a, CAP, b, 0, CAP + 60), CAP + 60, xat(a, CAP, b, 0, -60), -60, c);
        d.line(xat(b, CAP, a, 0, CAP + 60), CAP + 60, xat(b, CAP, a, 0, -60), -60, c);
        return R + 24;
    };
    m['Y'] = [=](D& d) {
        double L = 14, R = L + 640 + d.ex, xm = (L + R) / 2, ym = CAP * 0.42, a = L + d.hx, b = R - d.hx;
        Clip c{-1e9, -1e9, 1e9, CAP};
        d.s(d.P().M(xat(a, CAP, xm, ym, CAP + 60), CAP + 60).L(xm, ym).L(xat(b, CAP, xm, ym, CAP + 60), CAP + 60), c, 6);
        d.stem(xm, 0, ym);
        return R + 14;
    };
    m['Z'] = [=](D& d) {
        double L = 44, R = L + 540 + d.ex;
        d.s(d.P().M(L + 10, CAP - d.hy).L(R - d.hx * 0.4, CAP - d.hy).L(L + d.hx * 0.4, d.hy).L(R, d.hy), Clip{L, 0, R, CAP}, 10);
        return R + 44;
    };
    m['K'] = [=](D& d) {
        double L = 78, R = L + 562 + d.ex, x0 = L + d.hx;
        d.stem(x0, 0, CAP);
        Clip c{-1e9, 0, R, CAP};
        double ax0 = R - d.hx * 0.7, ay0 = CAP, ax1 = x0 + d.hx * 0.3, ay1 = CAP * 0.30;
        d.line(xat(ax0, ay0, ax1, ay1, CAP + 60), CAP + 60, ax1, ay1, c);
        double lx = x0 + (R - x0) * 0.30, ly = ay1 + (ay0 - ay1) * (lx - ax1) / (ax0 - ax1);
        d.line(lx, ly, R + 34, -60, c);
        return R + 22;
    };
    m['M'] = [=](D& d) {
        double L = 78, R = L + 722 + d.ex * 1.5, x0 = L + d.hx, x1 = R - d.hx, xm = (L + R) / 2;
        Path p;
        p.M(x0, 0).L(x0, CAP + 40).L(xm, 70).L(x1, CAP + 40).L(x1, 0);
        d.s(p, Clip{-1e9, -1e9, 1e9, CAP}, 8);
        return R + 78;
    };
    m['N'] = [=](D& d) {
        double L = 78, R = L + 582 + d.ex, x0 = L + d.hx, x1 = R - d.hx;
        Path p;
        p.M(x0, 0).L(x0, CAP + 40).L(x1, -40).L(x1, CAP);
        d.s(p, Clip{-1e9, 0, 1e9, CAP}, 8);
        return R + 78;
    };
    m['U'] = [=](D& d) {
        double L = 74, R = L + 578 + d.ex, x0 = L + d.hx, x1 = R - d.hx, xm = (x0 + x1) / 2, rx = xm - x0, ry = rx * 0.92;
        double cyb = d.hy - O + ry;
        Path p;
        p.M(x0, CAP).L(x0, cyb).A(xm, cyb, rx, ry, 180, 360).L(x1, CAP);
        d.s(p);
        return R + 74;
    };
    m['J'] = [=](D& d) {
        double L = 30, R = L + 452 + d.ex, x0 = L + d.hx, x1 = R - d.hx, xm = (x0 + x1) / 2, rx = x1 - xm, ry = rx * 0.95;
        double cyb = d.hy - O + ry;
        Path p;
        p.M(x1, CAP).L(x1, cyb).A(xm, cyb, rx, ry, 0, -162);
        d.s(p);
        return R + 70;
    };

    // ---------------------------------------------------------------- digits (tabular)
    const double DW = 600;
    auto dbox = [](D& d) { return std::pair<double, double>{64, 64 + 472 + d.ex}; };
    m['0'] = [=](D& d) { auto [L, R] = dbox(d); d.bowl(L, R, -O, CAP + O, 2.35); return DW + d.ex; };
    m['1'] = [=](D& d) {
        double xs = 330 + d.ex * 0.5;
        d.s(d.P().M(xs - 170, CAP - 150).L(xs, CAP + 10).L(xs, 0), Clip{-1e9, -1e9, 1e9, CAP}, 6);
        return DW + d.ex;
    };
    m['2'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double x0 = L + d.hx, x1 = R - d.hx, top = CAP + O - d.hy, ry = CAP * 0.27, cy = top - ry, cx = (x0 + x1) / 2, rx = (x1 - x0) / 2;
        Path p;
        p.A(cx, cy, rx, ry, 158, -10).C(x1 - 20, cy - ry * 0.95, L + 60, CAP * 0.28, L + d.hx * 0.6, d.hy).L(R, d.hy);
        d.s(p, Clip{L, 0, R, 1e9}, 8);
        return DW + d.ex;
    };
    m['3'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double x0 = L + d.hx, x1 = R - d.hx, top = CAP + O - d.hy, bot = d.hy - O, ym = CAP * 0.555;
        double rxu = (x1 - x0) / 2 - 18, ryu = (top - ym) / 2, cyu = ym + ryu, cxu = (x0 + x1) / 2 - 4;
        double rxl = (x1 - x0) / 2, ryl = (ym - bot) / 2, cyl = bot + ryl, cxl = (x0 + x1) / 2;
        Path u;
        u.A(cxu, cyu, rxu, ryu, 152, -90).L(cxu - 70, ym);
        d.s(u);
        Path l;
        l.M(cxl - 70, ym).L(cxl, ym).A(cxl, cyl, rxl, ryl, 90, -152);
        d.s(l);
        return DW + d.ex;
    };
    m['4'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double xs = R - 118, yb = CAP * 0.27;
        d.s(d.P().M(xs + 10, CAP + 10).L(L + d.hx * 0.5, yb).L(R + 10, yb), Clip{L, -1e9, R + 10, CAP}, 8);
        d.stem(xs, 0, CAP * 0.62);
        return DW + d.ex;
    };
    m['5'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double x0 = L + d.hx, x1 = R - d.hx, bot = d.hy - O;
        double ryl = CAP * 0.31, cyl = bot + ryl, rxl = (x1 - x0) / 2, cxl = (x0 + x1) / 2;
        Path p;
        p.M(R - 18, CAP - d.hy).L(x0 + 26, CAP - d.hy).L(x0 + 6, CAP * 0.47).C(x0 + 60, cyl + ryl - 6, cxl - 50, cyl + ryl, cxl, cyl + ryl)
            .A(cxl, cyl, rxl, ryl, 90, -154);
        d.s(p);
        return DW + d.ex;
    };
    m['6'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double x0 = L + d.hx, x1 = R - d.hx, bot = d.hy - O, top = CAP + O - d.hy;
        double ryb = CAP * 0.32, cyb = bot + ryb, cx = (x0 + x1) / 2, rx = (x1 - x0) / 2;
        Path b;
        b.A(cx, cyb, rx, ryb, 0, 360).Z();
        d.s(b);
        Path u;
        double ryu = CAP * 0.5, cyu = top - ryu;
        u.A(cx + 4, cyu, rx - 4, ryu, 52, 180).L(x0, cyb);
        d.s(u);
        return DW + d.ex;
    };
    m['9'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double x0 = L + d.hx, x1 = R - d.hx, bot = d.hy - O, top = CAP + O - d.hy;
        double ryb = CAP * 0.32, cyb = top - ryb, cx = (x0 + x1) / 2, rx = (x1 - x0) / 2;
        Path b;
        b.A(cx, cyb, rx, ryb, 0, 360).Z();
        d.s(b);
        Path u;
        double ryu = CAP * 0.5, cyu = bot + ryu;
        u.A(cx - 4, cyu, rx - 4, ryu, 232, 360).L(x1, cyb);
        d.s(u);
        return DW + d.ex;
    };
    m['7'] = [=](D& d) {
        auto [L, R] = dbox(d);
        d.s(d.P().M(L + 4, CAP - d.hy).L(R - d.hx * 0.3, CAP - d.hy).L(L + 150, -20), Clip{L, 0, R, CAP}, 8);
        return DW + d.ex;
    };
    m['8'] = [=](D& d) {
        auto [L, R] = dbox(d);
        double ym = CAP * 0.555;
        d.bowl(L + 24, R - 24, ym - d.hy, CAP + O, 2.2);
        d.bowl(L, R, -O, ym + d.hy, 2.2);
        return DW + d.ex;
    };

    // ---------------------------------------------------------------- punctuation & symbols
    m['.'] = [=](D& d) { d.dot(125 + d.ex * 0.3, d.dot_r()); return 250 + d.ex * 0.6; };
    m[','] = [=](D& d) {
        double x = 125 + d.ex * 0.3, r = d.dot_r();
        d.dot(x, r);
        d.s(d.P().M(x + r * 0.5, r * 0.6).L(x - 30, -128), Pen{d.hx * 0.95, d.hy * 0.95});
        return 250 + d.ex * 0.6;
    };
    m[':'] = [=](D& d) { double x = 125 + d.ex * 0.3; d.dot(x, d.dot_r()); d.dot(x, XH - d.dot_r()); return 250 + d.ex * 0.6; };
    m[';'] = [=](D& d) {
        double x = 125 + d.ex * 0.3, r = d.dot_r();
        d.dot(x, XH - r);
        d.dot(x, r);
        d.s(d.P().M(x + r * 0.5, r * 0.6).L(x - 30, -128), Pen{d.hx * 0.95, d.hy * 0.95});
        return 250 + d.ex * 0.6;
    };
    m['!'] = [=](D& d) { double x = 130 + d.ex * 0.3; d.stem(x, CAP * 0.30, CAP); d.dot(x, d.dot_r()); return 260 + d.ex * 0.6; };
    m['?'] = [=](D& d) {
        double L = 40, R = L + 400 + d.ex, x0 = L + d.hx, x1 = R - d.hx, top = CAP + O - d.hy, cx = (x0 + x1) / 2;
        double ry = CAP * 0.24, cy = top - ry;
        Path p;
        p.A(cx, cy, (x1 - x0) / 2, ry, 160, -40).C(x1 - 40, cy - ry - 10, cx, cy - ry - 30, cx, CAP * 0.3);
        d.s(p);
        d.dot(cx, d.dot_r());
        return R + 40;
    };
    m['\''] = [=](D& d) { double x = 110 + d.ex * 0.3; d.stem(x, CAP - 230, CAP + 20, Clip{-1e9, -1e9, 1e9, CAP}); return 220 + d.ex * 0.6; };
    m['"'] = [=](D& d) {
        double x = 110 + d.ex * 0.3;
        d.stem(x, CAP - 230, CAP);
        d.stem(x + 150, CAP - 230, CAP);
        return 370 + d.ex;
    };
    m['`'] = [=](D& d) { d.line(150, CAP + 20, 250, CAP - 120, Clip{-1e9, -1e9, 1e9, CAP}); return 400; };
    m['-'] = [=](D& d) { d.bar(60, 340 + d.ex, CAP * 0.40); return 400 + d.ex; };
    m[0x2010] = m['-'];
    m[0x2011] = m['-'];
    m[0xAD] = m['-'];
    m[0x2013] = [=](D& d) { d.bar(60, 540, CAP * 0.40); return 600; };
    m[0x2014] = [=](D& d) { d.bar(60, 940, CAP * 0.40); return 1000; };
    m[0x2212] = [=](D& d) { d.bar(70, 530, CAP * 0.46); return 600; };
    m['_'] = [=](D& d) { d.bar(20, 520, -120 + d.hy); return 540; };
    auto paren = [=](D& d, bool right) {
        double w = 330 + d.ex * 0.4, cx = right ? -40.0 : w + 40, rx = 200 + d.ex * 0.3, top = ASC + 40, bot = DSC + 10;
        double cy = (top + bot) / 2, ry = (top - bot) / 2 - d.hy;
        Path p;
        if (right) p.A(cx, cy, rx, ry, 62, -62, 2.0);
        else p.A(cx, cy, rx, ry, 118, 242, 2.0);
        d.s(p);
        return w;
    };
    m['('] = [=](D& d) { return paren(d, false); };
    m[')'] = [=](D& d) { return paren(d, true); };
    m['['] = [=](D& d) {
        double x0 = 100 + d.hx, top = ASC + 40, bot = DSC + 10;
        d.s(d.P().M(270, top - d.hy).L(x0, top - d.hy).L(x0, bot + d.hy).L(270, bot + d.hy));
        return 320 + d.ex * 0.4;
    };
    m[']'] = [=](D& d) {
        double w = 320 + d.ex * 0.4, x1 = w - 100 - d.hx, top = ASC + 40, bot = DSC + 10;
        d.s(d.P().M(50, top - d.hy).L(x1, top - d.hy).L(x1, bot + d.hy).L(50, bot + d.hy));
        return w;
    };
    auto brace = [=](D& d, bool right) {
        double w = 340 + d.ex * 0.4, top = ASC + 40 - d.hy, bot = DSC + 10 + d.hy, mid = (top + bot) / 2;
        double xs = 170, xe = 280, xt = 70;  // stem, ends, tip
        Path p;
        p.M(xe, top).C(xs - 10, top, xs, top - 30, xs, top - 110).L(xs, mid + 90).C(xs, mid + 30, xs - 30, mid, xt, mid)
            .C(xs - 30, mid, xs, mid - 30, xs, mid - 90).L(xs, bot + 110).C(xs, bot + 30, xs - 10, bot, xe, bot);
        if (right)
            for (auto& q : p.p) q.x = w - q.x;
        d.s(p);
        return w;
    };
    m['{'] = [=](D& d) { return brace(d, false); };
    m['}'] = [=](D& d) { return brace(d, true); };
    m['/'] = [=](D& d) { d.line(40, DSC + 40, 340, ASC + 10, Clip{-1e9, DSC + 40, 1e9, ASC + 10}); return 380; };
    m['\\'] = [=](D& d) { d.line(340, DSC + 40, 40, ASC + 10, Clip{-1e9, DSC + 40, 1e9, ASC + 10}); return 380; };
    m['|'] = [=](D& d) { d.stem(140, DSC, ASC + 40); return 280; };
    m['+'] = [=](D& d) { d.bar(80, 520, 300); d.stem(300, 80, 520); return 600; };
    m['='] = [=](D& d) { d.bar(80, 520, 390); d.bar(80, 520, 210); return 600; };
    m['<'] = [=](D& d) { d.s(d.P().M(510, 520).L(100, 300).L(510, 80), {}, 6); return 600; };
    m['>'] = [=](D& d) { d.s(d.P().M(90, 520).L(500, 300).L(90, 80), {}, 6); return 600; };
    m['~'] = [=](D& d) {
        Path p;
        p.M(90, 260).C(130, 360, 200, 370, 300, 300).C(400, 230, 470, 240, 510, 340);
        d.s(p);
        return 600;
    };
    m['^'] = [=](D& d) { d.s(d.P().M(110, 390).L(300, CAP).L(490, 390), Clip{-1e9, -1e9, 1e9, CAP}, 6); return 600; };
    m['*'] = [=](D& d) {
        double cx = 250, cy = CAP - 170, r = 150;
        for (int i = 0; i < 3; i++) {
            double a = (90 + i * 60) * kPi / 180;
            d.line(cx + r * std::cos(a), cy + r * std::sin(a), cx - r * std::cos(a), cy - r * std::sin(a));
        }
        return 500;
    };
    m['#'] = [=](D& d) {
        Clip c{-1e9, 0, 1e9, CAP};
        d.line(220, 0, 290, CAP, c);
        d.line(410, 0, 480, CAP, c);
        d.bar(70, 600, 480);
        d.bar(50, 580, 250);
        return 660;
    };
    m['%'] = [=](D& d) {
        Pen thin{d.hx * 0.85, d.hy * 0.85};
        Path a;
        a.A(180, 560, 105, 150, 0, 360).Z();
        d.s(a, thin);
        Path b;
        b.A(590, 170, 105, 150, 0, 360).Z();
        d.s(b, thin);
        d.line(150, -20, 640, CAP + 20, Clip{-1e9, 0, 1e9, CAP});
        return 780;
    };
    m['&'] = [=](D& d) {
        double bot = d.hy - O;
        Path p;
        p.M(600, 0).L(260, 440).C(180, 520, 170, 560, 170, 610).C(170, 690, 230, CAP + O - d.hy, 300, CAP + O - d.hy)
            .C(370, CAP + O - d.hy, 420, 690, 420, 620).C(420, 540, 340, 490, 260, 440).C(150, 370, 90, 300, 90, 200)
            .C(90, 80, 180, bot, 300, bot).C(420, bot, 520, 90, 560, 280);
        d.s(p);
        return 680;
    };
    m['@'] = [=](D& d) {
        Pen thin{d.hx * 0.82, d.hy * 0.82};
        double cx = 480, cy = 250, sx = cx + 120;
        Path in;
        in.A(cx - 8, cy + 10, 128 - thin.hx * 0.2, 160, 0, 360, 2.1).Z();
        d.s(in, thin);
        double rx = 390, ry = 410, a0 = -24 * kPi / 180;
        double ex = cx + rx * std::cos(a0), ey = cy + 40 + ry * std::sin(a0);
        Path out;
        out.M(sx, cy + 175).L(sx, cy - 70).C(sx, cy - 150, sx + 60, cy - 170, sx + 110, cy - 160).C(sx + 160, cy - 150, ex - 20, ey - 40, ex, ey)
            .A(cx, cy + 40, rx, ry, -24, 300, 2.0);
        d.s(out, thin);
        return 960;
    };
    m['$'] = [=](D& d) {
        // the S, slimmer, with a bar through it
        double L = 64, R = L + 460, x0 = L + d.hx, x1 = R - d.hx, top = CAP + O - d.hy - 10, bot = d.hy - O + 10;
        double ym = CAP * 0.52, xm = (x0 + x1) / 2;
        Path p;
        p.superness = 2.15;
        p.A(xm - 6, ym + (top - ym) / 2, (x1 - x0) / 2 - 10, (top - ym) / 2, 24, 270).A(xm, bot + (ym - bot) / 2, (x1 - x0) / 2, (ym - bot) / 2, 90, -156);
        d.s(p);
        d.stem(xm, -100, CAP + 90);
        return 600;
    };
    m[0x2026] = [=](D& d) {  // …
        double r = d.dot_r();
        for (int i = 0; i < 3; i++) d.dot(150 + i * 250, r);
        return 800;
    };
    m[0xB7] = [=](D& d) { d.dot(125, CAP * 0.45); return 250; };
    m[0x2022] = [=](D& d) { d.dot(250, CAP * 0.45, 1.9); return 500; };
    m[0xD7] = [=](D& d) { d.line(130, 130, 470, 470); d.line(130, 470, 470, 130); return 600; };
    m[0xF7] = [=](D& d) { d.bar(80, 520, 300); d.dot(300, 520); d.dot(300, 80); return 600; };
    m[0xB1] = [=](D& d) { d.bar(80, 520, 360); d.stem(300, 140, 580); d.bar(80, 520, d.hy); return 600; };
    m[0xB0] = [=](D& d) { Path p; p.A(200, CAP - 120, 105, 105, 0, 360, 2.0).Z(); d.s(p, Pen{d.hx * 0.8, d.hy * 0.8}); return 400; };
    m[0x2192] = [=](D& d) {  // →
        d.bar(80, 760, 300);
        d.s(d.P().M(520, 540).L(780, 300).L(520, 60), {}, 4);
        return 860;
    };
    m[0x2190] = [=](D& d) {  // ←
        d.bar(100, 780, 300);
        d.s(d.P().M(340, 540).L(80, 300).L(340, 60), {}, 4);
        return 860;
    };
    m[0x2191] = [=](D& d) { d.stem(300, -10, CAP); d.s(d.P().M(80, CAP - 220).L(300, CAP + 10).L(520, CAP - 220), {}, 4); return 600; };
    m[0x2193] = [=](D& d) { d.stem(300, 0, CAP + 10); d.s(d.P().M(80, 220).L(300, -10).L(520, 220), {}, 4); return 600; };
    m[0x2713] = [=](D& d) { d.s(d.P().M(90, 330).L(270, 120).L(600, CAP), {}, 4); return 700; };  // ✓
    m[0x2714] = [=](D& d) { d.s(d.P().M(90, 330).L(270, 120).L(600, CAP), Pen{d.hx * 1.5, d.hy * 1.5}, {}); return 700; };  // ✔
    m[0x2715] = [=](D& d) { d.line(130, 70, 530, 470); d.line(130, 470, 530, 70); return 660; };  // ✕
    m[0x2318] = [=](D& d) {  // ⌘
        Pen thin{d.hx * 0.8, d.hy * 0.8};
        double a = 260, b = 500, r = 85;
        Path p;
        p.M(a, b).L(a, a).L(b, a).L(b, b).Z();
        d.s(p, thin);
        for (auto [cx, cy, a0] : std::vector<std::tuple<double, double, double>>{{a - r, b + r, -90}, {b + r, b + r, 180}, {b + r, a - r, 90}, {a - r, a - r, 0}}) {
            Path q;
            q.A(cx, cy, r, r, a0, a0 + 270, 2.0);
            d.s(q, thin);
        }
        return 760;
    };
    m[0x21E7] = [=](D& d) {  // ⇧
        Pen thin{d.hx * 0.8, d.hy * 0.8};
        Path p;
        p.M(240, 0).L(460, 0).L(460, 300).L(620, 300).L(350, CAP).L(80, 300).L(240, 300).Z();
        d.s(p, thin);
        return 700;
    };
    m[0x2325] = [=](D& d) {  // ⌥
        d.s(d.P().M(60, CAP - 50).L(250, CAP - 50).L(470, 50).L(660, 50));
        d.bar(430, 660, CAP - 50);
        return 720;
    };
    m[0x2303] = [=](D& d) { d.s(d.P().M(120, 380).L(330, CAP - 20).L(540, 380), {}, 6); return 660; };  // ⌃
    m[0x23CE] = [=](D& d) { d.s(d.P().M(560, CAP).L(560, 220).L(120, 220)); d.s(d.P().M(280, 400).L(110, 220).L(280, 40), {}, 4); return 680; };
    m[0x232B] = [=](D& d) {  // ⌫
        Pen thin{d.hx * 0.8, d.hy * 0.8};
        Path p; p.M(260, 40).L(720, 40).L(720, 560).L(260, 560).L(60, 300).Z();
        d.s(p, thin);
        d.line(380, 200, 580, 400, {}); d.line(380, 400, 580, 200, {});
        return 780;
    };
    m[0xA9] = [=](D& d) {  // ©
        Pen thin{d.hx * 0.75, d.hy * 0.75};
        Path o; o.A(400, CAP / 2, 330, 360, 0, 360, 2.0).Z(); d.s(o, thin);
        Path c; c.A(410, CAP / 2, 140, 170, 40, 320, 2.1); d.s(c, thin);
        return 800;
    };
    m[0xAE] = [=](D& d) {  // ®
        Pen thin{d.hx * 0.7, d.hy * 0.7};
        Path o; o.A(400, CAP / 2, 330, 360, 0, 360, 2.0).Z(); d.s(o, thin);
        double x0 = 300, ym = 360, top = 560;
        Path p; p.M(x0, 160).L(x0, top).L(420, top).A(420, (top + ym) / 2, 90, (top - ym) / 2, 90, -90, 2.2).L(x0, ym); d.s(p, thin);
        d.s(d.P().M(420, ym).L(520, 160), thin);
        return 800;
    };
    m[0x2122] = [=](D& d) {  // ™
        Pen thin{d.hx * 0.7, d.hy * 0.7};
        double top = CAP, b = CAP - 300;
        d.s(d.P().M(40, top - thin.hy).L(300, top - thin.hy), thin);
        d.s(d.P().M(170, top - thin.hy).L(170, b), thin);
        d.s(d.P().M(380, b).L(380, top + 20).L(520, b + 40).L(660, top + 20).L(660, b), thin, Clip{-1e9, -1e9, 1e9, top});
        return 740;
    };
    auto quote = [=](D& d, double x, double y, bool open) {
        double r = d.dot_r() * 0.92;
        if (open) {  // comma shape turned around, sitting high
            d.dot(x, y + r * 0.6, 0.92);
            d.s(d.P().M(x - r * 0.5, y + r * 0.9).L(x + 26, y + 150), Pen{d.hx * 0.9, d.hy * 0.9});
        } else {
            d.dot(x, y + r, 0.92);
            d.s(d.P().M(x + r * 0.5, y + r * 0.6).L(x - 26, y - 120), Pen{d.hx * 0.9, d.hy * 0.9});
        }
    };
    m[0x2018] = [=](D& d) { quote(d, 120, CAP - 190, true); return 240; };
    m[0x2019] = [=](D& d) { quote(d, 120, CAP - 100, false); return 240; };
    m[0x201A] = [=](D& d) { quote(d, 120, 0, false); return 240; };
    m[0x201C] = [=](D& d) { quote(d, 120, CAP - 190, true); quote(d, 270, CAP - 190, true); return 390; };
    m[0x201D] = [=](D& d) { quote(d, 120, CAP - 100, false); quote(d, 270, CAP - 100, false); return 390; };
    m[0x201E] = [=](D& d) { quote(d, 120, 0, false); quote(d, 270, 0, false); return 390; };
    m[0x2039] = [=](D& d) { d.s(d.P().M(270, 470).L(100, 270).L(270, 70), {}, 6); return 360; };
    m[0x203A] = [=](D& d) { d.s(d.P().M(90, 470).L(260, 270).L(90, 70), {}, 6); return 360; };
    m[0xAB] = [=](D& d) { d.s(d.P().M(270, 470).L(100, 270).L(270, 70), {}, 6); d.s(d.P().M(470, 470).L(300, 270).L(470, 70), {}, 6); return 560; };
    m[0xBB] = [=](D& d) { d.s(d.P().M(90, 470).L(260, 270).L(90, 70), {}, 6); d.s(d.P().M(290, 470).L(460, 270).L(290, 70), {}, 6); return 560; };
    m[0xA1] = [=](D& d) { double x = 130; d.stem(x, DSC + 30, XH - 200); d.dot(x, XH - d.dot_r()); return 260; };
    m[0xBF] = [=](D& d) {
        double L = 40, R = L + 400, x0 = L + d.hx, x1 = R - d.hx, cx = (x0 + x1) / 2, bot = DSC + d.hy - O + 20, ry = CAP * 0.24, cy = bot + ry;
        Path p;
        p.M(cx, XH - 210).C(cx, cy + ry + 30, x0 + 40, cy + ry + 10, x0, cy).A(cx, cy, (x1 - x0) / 2, ry, 180, 380);
        d.s(p);
        d.dot(cx, XH - d.dot_r());
        return R + 40;
    };
    m[0x20AC] = [=](D& d) {  // €
        double L = 40, R = 600, cx = 360, cy = CAP / 2, rx = 260 - d.hx, ry = CAP / 2 + O - d.hy;
        d.s(d.P().A(cx, cy, rx, ry, 42, 318, 2.2));
        d.bar(L, 420, CAP * 0.58);
        d.bar(L, 400, CAP * 0.40);
        return R + 40;
    };
    m[0xA3] = [=](D& d) {  // £
        Path p;
        p.M(470, 610).C(450, 700, 400, CAP + O - d.hy, 320, CAP + O - d.hy).C(230, CAP + O - d.hy, 180, 680, 180, 580).L(180, 180)
            .C(180, 90, 140, 40, 80, d.hy).L(520, d.hy);
        d.s(p);
        d.bar(80, 400, CAP * 0.44);
        return 600;
    };
    m[0xA5] = [=](D& d) {  // ¥
        double xm = 300, ym = CAP * 0.45;
        d.s(d.P().M(40, CAP + 60).L(xm, ym).L(560, CAP + 60), Clip{-1e9, -1e9, 1e9, CAP}, 6);
        d.stem(xm, 0, ym);
        d.bar(110, 490, CAP * 0.34);
        d.bar(110, 490, CAP * 0.17);
        return 600;
    };
    m[0xA2] = [=](D& d) {  // ¢
        double cx = 300, cy = XH / 2 + 60, rx = 220 - d.hx, ry = XH / 2 + O - d.hy;
        d.s(d.P().A(cx, cy, rx, ry, 36, 324));
        d.stem(cx + 10, cy - ry - 100, cy + ry + 100);
        return 600;
    };
    m[0xA7] = [=](D& d) {  // §
        Pen thin{d.hx * 0.9, d.hy * 0.9};
        Path p;
        p.superness = 2.1;
        p.A(300, CAP - 130, 150, 120, 20, 270).A(300, CAP - 370, 180, 120, 90, -90).A(300, 130, 150, 120, 90, -160);
        d.s(p, thin);
        return 600;
    };
    m[0xB6] = [=](D& d) {  // ¶
        d.stem(330, DSC + 40, CAP);
        d.stem(470, DSC + 40, CAP);
        Path p;
        p.M(520, CAP - d.hy).L(250, CAP - d.hy).A(250, CAP - 180, 160, 180 - d.hy, 90, 270, 2.2).L(330, CAP - 360 + d.hy);
        d.s(p);
        return 600;
    };
    m[0xAC] = [=](D& d) { d.s(d.P().M(80, 360).L(510, 360).L(510, 150)); return 600; };  // ¬
    m[0xB5] = [=](D& d) {  // µ
        double adv = m.at('u')(d);
        double x0 = 64 + d.hx;
        d.stem(x0, DSC, XH);
        return adv;
    };
    m[0x25B2] = [=](D& d) { d.fill({{80, 0}, {520, 0}, {300, CAP * 0.8}}); return 600; };
    m[0x25BC] = [=](D& d) { d.fill({{80, CAP * 0.8}, {520, CAP * 0.8}, {300, 0}}); return 600; };
    m[0x25B6] = [=](D& d) { d.fill({{100, 30}, {100, 570}, {540, 300}}); return 640; };
    m[0x25C0] = [=](D& d) { d.fill({{540, 30}, {540, 570}, {100, 300}}); return 640; };
    m[0x25C6] = [=](D& d) { d.fill({{300, 20}, {560, 300}, {300, 580}, {40, 300}}); return 600; };
    m[0x25CF] = [=](D& d) { d.fill(superellipse(300, 300, 240, 240, 2.0, 64)); return 600; };
    m[0x25CB] = [=](D& d) { Path p; p.A(300, 300, 240 - d.hx * 0.8, 240 - d.hy * 0.8, 0, 360, 2.0).Z(); d.s(p, Pen{d.hx * 0.8, d.hy * 0.8}); return 600; };
    m[0x25A0] = [=](D& d) { d.fill({{80, 40}, {520, 40}, {520, 480}, {80, 480}}); return 600; };
    m[0x25A1] = [=](D& d) { Path p; p.M(80 + d.hx, 40 + d.hy).L(520 - d.hx, 40 + d.hy).L(520 - d.hx, 480 - d.hy).L(80 + d.hx, 480 - d.hy).Z(); d.s(p); return 600; };
    m[0xFFFD] = [=](D& d) {  // replacement character: diamond with a hole shaped like "?"
        d.fill({{350, -60}, {680, CAP / 2}, {350, CAP + 60}, {20, CAP / 2}});
        return 700;
    };
    return m;
}

// Accents: drawn centred on x=0, sitting on y=0 (the caller lifts them to x-height or cap height).
void draw_mark(D& d, Mark mk, double cx, double base, bool cap) {
    double s = cap ? 0.82 : 1.0;  // flatter over capitals
    double sx = d.mx, bx = d.bx;
    (void)sx;
    (void)bx;
    double gap = cap ? 50 : 70;
    double y0 = base + gap;
    auto L = [&](double x0, double ya, double x1, double yb, Clip c = {}) { d.line(cx + x0, y0 + ya * s, cx + x1, y0 + yb * s, c); };
    Clip top{-1e9, y0, 1e9, y0 + 170 * s};
    switch (mk) {
        case M_acute: L(-40, 0, 80, 170, top); break;
        case M_grave: L(40, 0, -80, 170, top); break;
        case M_circumflex: d.s(d.P().M(cx - 130, y0 - 30).L(cx, y0 + 160 * s).L(cx + 130, y0 - 30), top, 6); break;
        case M_caron: d.s(d.P().M(cx - 130, y0 + 190 * s).L(cx, y0).L(cx + 130, y0 + 190 * s), Clip{-1e9, y0, 1e9, y0 + 160 * s}, 6); break;
        case M_tilde: {
            Path p;
            p.M(cx - 140, y0 + 20 * s).C(cx - 110, y0 + 110 * s, cx - 60, y0 + 120 * s, cx, y0 + 70 * s).C(cx + 60, y0 + 20 * s, cx + 110, y0 + 30 * s, cx + 140, y0 + 120 * s);
            d.s(p, Pen{d.hx * 0.9, d.hy * 0.9});
            break;
        }
        case M_macron: d.bar(cx - 130, cx + 130, y0 + 60 * s); break;
        case M_breve: { Path p; p.A(cx, y0 + 150 * s, 120, 120 * s, 200, 340, 2.0); d.s(p, Pen{d.hx * 0.9, d.hy * 0.9}); break; }
        case M_dotabove: d.dot(cx, y0 + d.dot_r()); break;
        case M_dieresis: d.dot(cx - 110, y0 + d.dot_r()); d.dot(cx + 110, y0 + d.dot_r()); break;
        case M_ring: { Path p; p.A(cx, y0 + 95 * s, 85, 85 * s, 0, 360, 2.0).Z(); d.s(p, Pen{d.hx * 0.75, d.hy * 0.75}); break; }
        case M_hungarumlaut: L(-90, 0, 0, 170, top); L(40, 0, 130, 170, top); break;
        case M_cedilla: {
            Path p;
            p.M(cx + 10, 0).L(cx - 10, -70).C(cx + 60, -80, cx + 100, -110, cx + 90, -150).C(cx + 80, -200, cx, -210, cx - 80, -190);
            d.s(p, Pen{d.hx * 0.8, d.hy * 0.8});
            break;
        }
        case M_ogonek: {
            Path p;
            p.M(cx + 40, 10).C(cx - 50, -40, cx - 60, -110, cx - 40, -150).C(cx - 10, -200, cx + 60, -200, cx + 100, -180);
            d.s(p, Pen{d.hx * 0.8, d.hy * 0.8});
            break;
        }
        case M_commabelow: d.dot(cx, -110); d.s(d.P().M(cx + 20, -100).L(cx - 25, -240), Pen{d.hx * 0.9, d.hy * 0.9}); break;
        case M_horn: d.s(d.P().M(cx, base - 40).C(cx + 60, base, cx + 80, base + 60, cx + 60, base + 130), Pen{d.hx * 0.9, d.hy * 0.9}); break;
    }
}

// ---------------------------------------------------------------- monospace
// Glyphs that need their own monospace drawing; everything else is the proportional
// design fitted (centred, and narrowed if it's too wide) into a 600-unit cell.
std::map<uint32_t, Design> mono_overrides() {
    std::map<uint32_t, Design> m;
    const double CAP = D::CAP, XH = D::XH, ASC = D::ASC, DSC = D::DSC;
    m['i'] = [=](D& d) {
        double x = 300 + 20;
        d.s(d.P().M(140, XH - d.hy).L(x, XH - d.hy).L(x, 0));
        d.bar(130, 490, d.hy);
        d.dot(x - 10, XH + 150 + d.dot_r());
        return 600.0;
    };
    m['l'] = [=](D& d) {
        double x = 300;
        Path p;
        p.M(130, ASC - d.hy).L(x, ASC - d.hy).L(x, 150).C(x, 60, x + 40, d.hy, x + 130, d.hy).L(500, d.hy);
        d.s(p);
        return 600.0;
    };
    m['j'] = [=](D& d) {
        double x = 380, bt = DSC + d.hy - D::O;
        Path p;
        p.M(150, XH - d.hy).L(x, XH - d.hy).L(x, -20).C(x, bt + 50, x - 50, bt, x - 130, bt).L(110, bt + 4);
        d.s(p);
        d.dot(x - 10, XH + 150 + d.dot_r());
        return 600.0;
    };
    m['I'] = [=](D& d) { d.bar(120, 480, CAP - d.hy); d.bar(120, 480, d.hy); d.stem(300, d.hy, CAP - d.hy); return 600.0; };
    m['r'] = [=](D& d) {
        double x0 = 150 + d.hx, top = XH + D::O - d.hy;
        d.stem(x0, 0, XH);
        double ry = 140, yj = top - ry * 1.3;
        Path p;
        p.M(x0, yj).C(x0, yj + (top - yj) * 0.6, x0 + 70, top, x0 + 170, top).C(x0 + 260, top, x0 + 300, top - 30, x0 + 320, top - 100);
        d.s(p);
        return 600.0;
    };
    m['f'] = [=](D& d) {
        double x0 = 250, topf = ASC + D::O - d.hy;
        Path p;
        p.M(x0, 0).L(x0, ASC - 170).C(x0, topf - 30, x0 + 50, topf, x0 + 140, topf).L(x0 + 270, topf - 10);
        d.s(p);
        d.bar(90, 500, XH - d.hy);
        return 600.0;
    };
    m['t'] = [=](D& d) {
        double x0 = 250, bot = d.hy - D::O;
        Path p;
        p.M(x0, 690).L(x0, 140).C(x0, bot + 30, x0 + 50, bot, x0 + 140, bot).L(x0 + 250, bot + 10);
        d.s(p);
        d.bar(90, 500, XH - d.hy);
        return 600.0;
    };
    m['1'] = [=](D& d) {
        double xs = 320;
        d.s(d.P().M(xs - 190, CAP - 150).L(xs, CAP + 10).L(xs, d.hy), Clip{-1e9, -1e9, 1e9, CAP}, 6);
        d.bar(120, 500, d.hy);
        return 600.0;
    };
    m['0'] = [=](D& d) {
        d.bowl(70, 530, -D::O, CAP + D::O, 2.35);
        d.line(205, 210, 395, 520);  // slashed zero, short
        return 600.0;
    };
    m['m'] = [=](D& d) {
        double L = 50, R = 550, x0 = L + d.hx * 0.8, x2 = R - d.hx * 0.8, x1 = (x0 + x2) / 2;
        Pen thin{d.hx * 0.8, d.hy};
        Pen save = d.pen;
        d.pen = thin;
        d.hx *= 0.8;
        d.stem(x0, 0, XH);
        d.arch(x0, x1, XH + D::O - d.hy, 0, 0);
        d.arch(x1, x2, XH + D::O - d.hy, 0, 0);
        d.pen = save;
        d.hx = save.hx;
        return 600.0;
    };
    m['w'] = [=](D& d) {
        double L = 30, R = 570, w = R - L;
        Pen save = d.pen;
        d.pen = Pen{d.hx * 0.85, d.hy};
        double t0 = L + d.pen.hx, q1 = L + w * 0.22, mid = L + w * 0.5, q3 = L + w * 0.78, t4 = R - d.pen.hx;
        Path p;
        p.M(xat(t0, XH, q1, 10, XH + 80), XH + 80).L(q1, 10).L(mid, XH - 120).L(q3, 10).L(xat(t4, XH, q3, 10, XH + 80), XH + 80);
        d.s(p, Clip{-1e9, 0, 1e9, XH}, 6);
        d.pen = save;
        return 600.0;
    };
    m['W'] = [=](D& d) {
        double L = 30, R = 570, w = R - L;
        Pen save = d.pen;
        d.pen = Pen{d.hx * 0.85, d.hy};
        double t0 = L + d.pen.hx, q1 = L + w * 0.22, mid = L + w * 0.5, q3 = L + w * 0.78, t4 = R - d.pen.hx;
        Path p;
        p.M(xat(t0, CAP, q1, 8, CAP + 80), CAP + 80).L(q1, 8).L(mid, CAP - 220).L(q3, 8).L(xat(t4, CAP, q3, 8, CAP + 80), CAP + 80);
        d.s(p, Clip{-1e9, 0, 1e9, CAP}, 6);
        d.pen = save;
        return 600.0;
    };
    m['M'] = [=](D& d) {
        double L = 60, R = 540, x0 = L + d.hx * 0.85, x1 = R - d.hx * 0.85, xm = 300;
        Pen save = d.pen;
        d.pen = Pen{d.hx * 0.85, d.hy};
        Path p;
        p.M(x0, 0).L(x0, CAP + 40).L(xm, 200).L(x1, CAP + 40).L(x1, 0);
        d.s(p, Clip{-1e9, -1e9, 1e9, CAP}, 8);
        d.pen = save;
        return 600.0;
    };
    return m;
}

double lw_half() { return 46; }

// Box drawing and block elements, filling the whole terminal cell.
bool draw_box(D& d, uint32_t cp) {
    const double W = 600, top = D::LINE_TOP, bot = D::LINE_BOT, cx = W / 2, cy = (top + bot) / 2;
    auto rect = [&](double x0, double y0, double x1, double y1) { d.fill({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}); };
    for (auto& b : kBox) {
        if (b.cp != cp) continue;
        double lw = 92, hw = 184, gap = 110;  // light, heavy half-widths are lw/2, hw/2; double: two light lines `gap` apart
        auto arm = [&](int dir, int w) {  // dir 0 up 1 down 2 left 3 right
            if (!w) return;
            double h = (w == 2 ? hw : lw) / 2;
            auto seg = [&](double off) {
                double ext = h;  // overlap the centre so arms join
                if (dir == 0) rect(cx - h + off, cy - ext, cx + h + off, top);
                if (dir == 1) rect(cx - h + off, bot, cx + h + off, cy + ext);
                if (dir == 2) rect(0, cy - h + off, cx + ext, cy + h + off);
                if (dir == 3) rect(cx - ext, cy - h + off, W, cy + h + off);
            };
            if (w == 3) { seg(-gap); seg(gap); }
            else seg(0);
        };
        if (b.dash) {
            int n = b.dash;
            double h = ((b.u == 2 || b.l == 2) ? hw : lw) / 2;
            bool horiz = b.l || b.r;
            double span = horiz ? W : top - bot, cell = span / n;
            for (int i = 0; i < n; i++) {
                double a = i * cell + cell * 0.18, e = (i + 1) * cell - cell * 0.18;
                if (horiz) rect(a, cy - h, e, cy + h);
                else rect(cx - h, bot + a, cx + h, bot + e);
            }
            return true;
        }
        arm(0, b.u);
        arm(1, b.d);
        arm(2, b.l);
        arm(3, b.r);
        return true;
    }
    if (cp >= 0x256D && cp <= 0x2570) {  // rounded corners ╭╮╯╰
        double r = 220;
        Pen p{lw_half(), lw_half()};
        Path q;
        bool right = cp == 0x256D || cp == 0x2570, down = cp == 0x256D || cp == 0x256E;
        double xe = right ? W + 10 : -10, ye = down ? bot - 10 : top + 10;
        double sx = right ? 1 : -1, sy = down ? -1 : 1;
        q.M(cx, ye).L(cx, cy + sy * r).C(cx, cy + sy * r * 0.45, cx + sx * r * 0.45, cy, cx + sx * r, cy).L(xe, cy);
        stroke(q, p, d.g->contours, Clip{0, bot, W, top});
        return true;
    }
    if (cp >= 0x2571 && cp <= 0x2573) {
        Pen p{46, 46};
        if (cp != 0x2572) stroke(Path().M(-20, bot - 40).L(W + 20, top + 40), p, d.g->contours, Clip{0, bot, W, top});
        if (cp != 0x2571) stroke(Path().M(-20, top + 40).L(W + 20, bot - 40), p, d.g->contours, Clip{0, bot, W, top});
        return true;
    }
    if (cp >= 0x2574 && cp <= 0x257F) {  // half lines
        static const int arms[12][4] = {{0, 0, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 0, 0}, {0, 0, 2, 0}, {2, 0, 0, 0},
                                        {0, 0, 0, 2}, {0, 2, 0, 0}, {0, 0, 1, 2}, {1, 2, 0, 0}, {0, 0, 2, 1}, {2, 1, 0, 0}};
        auto& a = arms[cp - 0x2574];
        double lw = 92, hw = 184;
        auto half = [&](int w) { return (w == 2 ? hw : lw) / 2; };
        if (a[0]) rect(cx - half(a[0]), cy, cx + half(a[0]), top);
        if (a[1]) rect(cx - half(a[1]), bot, cx + half(a[1]), cy);
        if (a[2]) rect(0, cy - half(a[2]), cx, cy + half(a[2]));
        if (a[3]) rect(cx, cy - half(a[3]), W, cy + half(a[3]));
        return true;
    }
    double H = top - bot;
    switch (cp) {
        case 0x2580: rect(0, cy, W, top); return true;               // ▀
        case 0x2584: rect(0, bot, W, cy); return true;               // ▄
        case 0x2588: rect(0, bot, W, top); return true;              // █
        case 0x258C: rect(0, bot, cx, top); return true;             // ▌
        case 0x2590: rect(cx, bot, W, top); return true;             // ▐
        case 0x2594: rect(0, top - H / 8, W, top); return true;      // ▔
        case 0x2595: rect(W - W / 8, bot, W, top); return true;      // ▕
        case 0x2596: rect(0, bot, cx, cy); return true;
        case 0x2597: rect(cx, bot, W, cy); return true;
        case 0x2598: rect(0, cy, cx, top); return true;
        case 0x259D: rect(cx, cy, W, top); return true;
        case 0x2599: rect(0, bot, cx, top); rect(cx, bot, W, cy); return true;
        case 0x259B: rect(0, bot, cx, top); rect(cx, cy, W, top); return true;
        case 0x259C: rect(0, cy, W, top); rect(cx, bot, W, cy); return true;
        case 0x259F: rect(cx, bot, W, top); rect(0, bot, cx, cy); return true;
        case 0x259A: rect(0, cy, cx, top); rect(cx, bot, W, cy); return true;
        case 0x259E: rect(cx, cy, W, top); rect(0, bot, cx, cy); return true;
        case 0x2591: case 0x2592: case 0x2593: {  // shades: a grid of squares
            int nx = 4, ny = 8;
            double frac = cp == 0x2591 ? 0.25 : cp == 0x2592 ? 0.5 : 0.75;
            double cw = W / nx, ch = H / ny, s = std::sqrt(frac);
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    double ox = i * cw + (j % 2 ? cw / 2 : 0);
                    double w = cw * s, h = ch * s;
                    double x0 = ox + (cw - w) / 2, y0 = bot + j * ch + (ch - h) / 2;
                    rect(std::max(0.0, x0), y0, std::min(W, x0 + w), y0 + h);
                }
            return true;
        }
    }
    if (cp >= 0x2581 && cp <= 0x2587) { rect(0, bot, W, bot + H * (cp - 0x2580) / 8); return true; }  // ▁..▇
    if (cp >= 0x2589 && cp <= 0x258F) { rect(0, bot, W * (0x2590 - cp) / 8, top); return true; }  // ▉..▏
    return false;
}

// ---------------------------------------------------------------- building fonts
struct Built { FontInfo info; std::vector<Glyph> glyphs; };

Built build(const Style& st) {
    auto des = designs();
    auto mono = st.mono ? mono_overrides() : std::map<uint32_t, Design>{};
    Built out;
    D d(st);
    // measure a design's horizontal extent with no transform
    auto run = [&](const Design& f, Glyph& g) {
        d.g = &g;
        g.contours.clear();
        g.advance = f(d);
    };
    auto make = [&](uint32_t cp, const Design& f, bool is_override) {
        Glyph g;
        g.cp = cp;
        d.mx = 1;
        d.bx = 0;
        run(f, g);
        if (st.mono && !is_override) {
            // fit into the 600 cell: centre the ink, squeeze if wider than 540
            double x0 = 1e9, x1 = -1e9;
            for (auto& c : g.contours) for (auto& p : c) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
            if (x0 > x1) { x0 = 0; x1 = g.advance; }
            double ink = x1 - x0, maxw = 548;
            double sc = 1;
            if (ink > maxw) sc = (maxw - 2 * st.hx) / std::max(1.0, ink - 2 * st.hx);  // stroke width is kept
            double cxi = (x0 + x1) / 2;
            d.mx = sc;
            d.bx = 300 - cxi * sc;
            run(f, g);
            g.advance = 600;
        } else if (st.mono) {
            g.advance = 600;
        }
        d.mx = 1;
        d.bx = 0;
        return g;
    };
    std::map<uint32_t, Glyph> glyphs;
    for (auto& [cp, f] : des) {
        if (!f) continue;
        if (mono.count(cp)) continue;
        glyphs[cp] = make(cp, f, false);
    }
    for (auto& [cp, f] : mono) glyphs[cp] = make(cp, f, true);
    // accented letters: base + marks
    for (auto& dc : kDecomp) {
        uint32_t base = dc.base;
        bool cap = base >= 'A' && base <= 'Z';
        bool soft = base == 'i' || base == 'j';
        if (soft && dc.marks[0] != M_ogonek && dc.marks[0] != M_cedilla && dc.marks[0] != M_commabelow) base = base == 'i' ? 0x131 : 0x237;
        if (!glyphs.count(base)) continue;
        Glyph g = glyphs[base];
        g.cp = dc.cp;
        d.g = &g;
        d.mx = 1;
        d.bx = 0;
        // centre: middle of the ink in the top half (stems of b/d/h... pull it off-centre otherwise)
        double x0 = 1e9, x1 = -1e9, ytop = -1e9;
        for (auto& c : g.contours) for (auto& p : c) {
            if (p.y > (cap ? D::CAP : D::XH) * 0.45) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
            ytop = std::max(ytop, p.y);
        }
        if (x0 > x1) { x0 = 0; x1 = g.advance; }
        double cx = (x0 + x1) / 2;
        if (base == 'd' || base == 'h' || base == 'k' || base == 'l' || base == 'L' || base == 't') cx = (x0 + x1) / 2;  // these get the mark beside/over the stem
        double basey = cap ? D::CAP : D::XH;
        if (base == 'l' || base == 'h' || base == 'k' || base == 'b' || base == 'd' || base == 't') basey = D::ASC;
        double stack = 0;
        for (auto mk : dc.marks) {
            bool below = mk == M_cedilla || mk == M_ogonek || mk == M_commabelow;
            double mx = cx;
            if (mk == M_ogonek) mx = x1 - 90;
            if (mk == M_cedilla || mk == M_commabelow) mx = (g.contours.empty() ? cx : cx);
            if (mk == M_horn) { draw_mark(d, mk, x1 - 30, cap ? D::CAP : D::XH, cap); continue; }
            if (mk == M_caron && (base == 'd' || base == 'l' || base == 't' || base == 'L')) {
                // a comma-like caron beside the stem: ď ľ ť Ľ
                double sx = x1 + 30, y = base == 't' ? 620 : (cap ? D::CAP : D::ASC) - 40;
                d.dot(sx, y);
                d.s(d.P().M(sx + 20, y + 10).L(sx - 30, y - 140), Pen{d.hx * 0.9, d.hy * 0.9});
                g.advance += 60;
                continue;
            }
            draw_mark(d, mk, mx, below ? 0 : basey + stack, cap);
            if (!below) stack += cap ? 150 : 190;
        }
        glyphs[dc.cp] = g;
    }
    // strokes through letters
    auto add_stroke = [&](uint32_t cp, uint32_t base, double y, double x0f, double x1f, double slant) {
        if (!glyphs.count(base)) return;
        Glyph g = glyphs[base];
        g.cp = cp;
        d.g = &g;
        double x0 = 1e9, x1 = -1e9;
        for (auto& c : g.contours) for (auto& p : c) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
        double a = x0 + (x1 - x0) * x0f, b = x0 + (x1 - x0) * x1f;
        d.line(a, y - slant, b, y + slant);
        glyphs[cp] = g;
    };
    add_stroke(0xD8, 'O', D::CAP / 2, 0.08, 0.92, 340);   // Ø
    add_stroke(0xF8, 'o', D::XH / 2, 0.06, 0.94, 250);    // ø
    add_stroke(0x141, 'L', D::CAP * 0.45, -0.18, 0.40, 90);  // Ł
    if (glyphs.count('l')) {  // ł: centred on the stem
        Glyph g = glyphs['l'];
        g.cp = 0x142;
        d.g = &g;
        double x0 = 1e9, x1 = -1e9;
        for (auto& c : g.contours) for (auto& p : c) if (p.y > D::XH) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
        double cxs = st.mono ? 300 : (x0 + x1) / 2;
        d.line(cxs - 120, D::XH * 0.55 - 80, cxs + 120, D::XH * 0.55 + 80);
        glyphs[0x142] = g;
    }
    add_stroke(0x110, 'D', D::CAP / 2, -0.12, 0.42, 0);   // Đ
    add_stroke(0xD0, 'D', D::CAP / 2, -0.12, 0.42, 0);    // Ð
    add_stroke(0x111, 'd', D::ASC - 130, 0.50, 1.18, 0);  // đ
    add_stroke(0x127, 'h', D::ASC - 130, -0.18, 0.50, 0); // ħ
    // ß (eszett), æ, Æ, œ, Œ, þ, Þ
    {
        Glyph g;
        g.cp = 0xDF;
        d.g = &g;
        double x0 = 64 + st.hx, top = D::ASC + D::O - st.hy;
        Path p;
        p.M(x0, 0).L(x0, D::ASC - 180).C(x0, top - 40, x0 + 60, top, x0 + 160, top).C(x0 + 270, top, x0 + 330, top - 70, x0 + 330, top - 160)
            .C(x0 + 330, top - 250, x0 + 250, D::XH + 40, x0 + 160, D::XH + 30).C(x0 + 300, D::XH + 10, x0 + 380, D::XH - 100, x0 + 380, D::XH / 2 - 20)
            .C(x0 + 380, 90, x0 + 300, st.hy - D::O, x0 + 200, st.hy - D::O).L(x0 + 140, st.hy);
        d.s(p);
        g.advance = x0 + 380 + st.hx + 50 + d.ex;
        if (st.mono) { g.advance = 600; }
        glyphs[0xDF] = g;
    }
    auto combine = [&](uint32_t cp, uint32_t a, uint32_t b, double overlap) {
        if (!glyphs.count(a) || !glyphs.count(b)) return;
        Glyph g = glyphs[a];
        g.cp = cp;
        double shift = g.advance - overlap;
        for (auto c : glyphs[b].contours) {
            for (auto& p : c) p.x += shift;
            g.contours.push_back(c);
        }
        g.advance = shift + glyphs[b].advance;
        if (st.mono) {  // squeeze into the cell
            double sc = 600 / g.advance;
            for (auto& c : g.contours) for (auto& p : c) p.x *= sc;
            g.advance = 600;
        }
        glyphs[cp] = g;
    };
    combine(0xE6, 'a', 'e', 150);
    combine(0xC6, 'A', 'E', 250);
    combine(0x153, 'o', 'e', 150);
    combine(0x152, 'O', 'E', 230);
    if (glyphs.count('p')) {  // þ: p with a tall stem
        Glyph g = glyphs['p'];
        g.cp = 0xFE;
        d.g = &g;
        d.mx = st.mono ? 1 : 1;
        double x = 1e9;
        for (auto& c : g.contours) for (auto& q : c) x = std::min(x, q.x);
        d.stem(x + st.hx, D::XH - 20, D::ASC);
        glyphs[0xFE] = g;
    }
    if (glyphs.count('P')) {  // Þ
        Glyph g;
        g.cp = 0xDE;
        d.g = &g;
        double L = 78, R = L + 486 + d.ex, x0 = L + st.hx, x1 = R - st.hx, yt = D::CAP * 0.78, yb = D::CAP * 0.22;
        double cy = (yt + yb) / 2, ry = (yt - yb) / 2, xc = x1 - ry;
        double sc = 1, bxx = 0;
        if (st.mono) { sc = 1; bxx = 300 - (L + R) / 2; }
        d.mx = sc;
        d.bx = bxx;
        d.stem(x0, 0, D::CAP);
        d.s(d.P().M(x0, yt).L(xc, yt).A(xc, cy, x1 - xc, ry, 90, -90, 2.3).L(x0, yb));
        d.mx = 1;
        d.bx = 0;
        g.advance = st.mono ? 600 : R + 48;
        glyphs[0xDE] = g;
    }
    if (st.mono)
        for (uint32_t cp = 0x2500; cp <= 0x259F; cp++) {
            Glyph g;
            g.cp = cp;
            g.advance = 600;
            d.g = &g;
            if (draw_box(d, cp)) glyphs[cp] = g;
        }
    // .notdef
    {
        Glyph g;
        g.cp = 0;
        d.g = &g;
        g.advance = st.mono ? 600 : 500;
        Path p;
        p.M(80, 0 + st.hy).L(420, st.hy).L(420, D::CAP - st.hy).L(80, D::CAP - st.hy).Z();
        d.s(p);
        glyphs[0] = g;
    }
    for (auto& [cp, g] : glyphs) out.glyphs.push_back(g);

    auto& fi = out.info;
    fi.family = st.mono ? "Tributary Mono" : "Tributary Sans";
    fi.style = st.weight >= 600 ? "Bold" : "Regular";
    fi.full = fi.family + (fi.style == "Regular" ? "" : " " + fi.style);
    fi.postscript = std::string(st.mono ? "TributaryMono-" : "TributarySans-") + fi.style;
    fi.weight = st.weight;
    fi.bold = st.weight >= 600;
    fi.mono = st.mono;
    fi.ascender = int(D::LINE_TOP);
    fi.descender = int(D::LINE_BOT);
    fi.cap_height = int(D::CAP);
    fi.x_height = int(D::XH);
    if (!st.mono) {
        // a little kerning for the pairs that need it most
        const char* round_lc = "aceoqdgs";
        auto kern = [&](uint32_t a, uint32_t b, int v) { fi.kerning[{a, b}] = int(v * (fi.bold ? 1.1 : 1)); };
        for (const char* c = round_lc; *c; c++) {
            kern('T', uint8_t(*c), -70); kern('V', uint8_t(*c), -40); kern('W', uint8_t(*c), -30); kern('Y', uint8_t(*c), -60);
            kern('F', uint8_t(*c), -20); kern('P', uint8_t(*c), -20);
        }
        for (char c : std::string("rvwxyzmnu")) { kern('T', uint8_t(c), -50); kern('Y', uint8_t(c), -40); }
        for (auto [a, b, v] : std::vector<std::tuple<char, char, int>>{
                 {'A', 'V', -50}, {'V', 'A', -50}, {'A', 'W', -35}, {'W', 'A', -35}, {'A', 'T', -50}, {'T', 'A', -50}, {'A', 'Y', -55},
                 {'Y', 'A', -55}, {'L', 'T', -80}, {'L', 'V', -70}, {'L', 'W', -50}, {'L', 'Y', -80}, {'P', 'A', -50}, {'F', 'A', -40},
                 {'A', 'v', -25}, {'A', 'w', -20}, {'A', 'y', -25}, {'r', '.', -50}, {'r', ',', -50}, {'T', '.', -70}, {'T', ',', -70},
                 {'V', '.', -60}, {'V', ',', -60}, {'Y', '.', -70}, {'Y', ',', -70}, {'P', '.', -80}, {'P', ',', -80}, {'F', '.', -60},
                 {'F', ',', -60}, {'f', 'f', -10}, {'v', '.', -40}, {'w', '.', -30}, {'y', '.', -40}, {'L', '\'', -90}, {'k', 'o', -15},
                 {'k', 'e', -15}, {'o', 'v', -10}, {'v', 'o', -10}, {'o', 'x', -10}, {'x', 'o', -10}, {'o', 'y', -10}, {'y', 'o', -10}})
            kern(uint8_t(a), uint8_t(b), v);
    }
    return out;
}

// ---------------------------------------------------------------- specimen (for checking designs)
void specimen(const std::vector<Built>& fonts, const std::vector<std::string>& lines, const char* out_path) {
    int W = 1400, lh = 0;
    std::vector<std::pair<int, std::string>> rows;  // size, text
    for (auto& l : lines) {
        int sz = std::atoi(l.c_str());
        auto sp = l.find(' ');
        rows.push_back({sz, l.substr(sp + 1)});
        lh += int(sz * 1.35) * int(fonts.size());
    }
    int H = lh + 20;
    std::vector<float> img(size_t(W) * H, 0);
    int y = 10;
    for (auto& [sz, text] : rows)
        for (auto& f : fonts) {
            std::map<uint32_t, const Glyph*> gm;
            for (auto& g : f.glyphs) gm[g.cp] = &g;
            double scale = sz / 1000.0, x = 10, base = y + sz * 0.969;
            // decode utf-8
            std::vector<uint32_t> cps;
            for (size_t i = 0; i < text.size();) {
                unsigned char c = text[i];
                uint32_t cp = c;
                int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                if (n > 1) { cp = c & (0x3F >> (n - 1)); for (int k = 1; k < n; k++) cp = cp << 6 | (text[i + k] & 0x3F); }
                cps.push_back(cp);
                i += n;
            }
            uint32_t prev = 0;
            for (auto cp : cps) {
                auto it = gm.find(cp);
                const Glyph* g = it == gm.end() ? gm[0] : it->second;
                auto kk = f.info.kerning.find({prev, cp});
                if (kk != f.info.kerning.end()) x += kk->second * scale;
                // rasterize with 4x4 supersampling, non-zero winding
                const int SS = 4;
                double gx0 = 1e9, gx1 = -1e9, gy0 = 1e9, gy1 = -1e9;
                for (auto& c : g->contours) for (auto& p : c) { gx0 = std::min(gx0, p.x); gx1 = std::max(gx1, p.x); gy0 = std::min(gy0, p.y); gy1 = std::max(gy1, p.y); }
                if (gx0 <= gx1) {
                    int px0 = int(std::floor(x + gx0 * scale)), px1 = int(std::ceil(x + gx1 * scale));
                    int py0 = int(std::floor(base - gy1 * scale)), py1 = int(std::ceil(base - gy0 * scale));
                    for (int py = py0; py < py1; py++)
                        for (int sy = 0; sy < SS; sy++) {
                            double yy = py + (sy + 0.5) / SS;
                            double fy = (base - yy) / scale;
                            std::vector<std::pair<double, int>> xs;
                            for (auto& c : g->contours)
                                for (size_t i = 0; i < c.size(); i++) {
                                    Pt a = c[i], b = c[(i + 1) % c.size()];
                                    if ((a.y <= fy) == (b.y <= fy)) continue;
                                    double t = (fy - a.y) / (b.y - a.y);
                                    xs.push_back({x + (a.x + (b.x - a.x) * t) * scale, b.y > a.y ? 1 : -1});
                                }
                            std::sort(xs.begin(), xs.end());
                            int wnd = 0;
                            for (size_t i = 0; i + 1 < xs.size(); i++) {
                                wnd += xs[i].second;
                                if (!wnd) continue;
                                for (int px = std::max(px0, 0); px < std::min(px1, W); px++)
                                    for (int sx = 0; sx < SS; sx++) {
                                        double xx = px + (sx + 0.5) / SS;
                                        if (xx >= xs[i].first && xx < xs[i + 1].first && py >= 0 && py < H) img[size_t(py) * W + px] += 1.0f / (SS * SS);
                                    }
                            }
                        }
                }
                x += g->advance * scale;
                prev = cp;
            }
            y += int(sz * 1.35);
        }
    FILE* fp = std::fopen(out_path, "wb");
    std::fprintf(fp, "P5 %d %d 255\n", W, H);
    for (float v : img) std::fputc(255 - int(std::min(1.0f, v) * 255), fp);
    std::fclose(fp);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<Style> styles = {{"Sans Regular", 43, 39, false, 400}, {"Sans Bold", 59, 53, false, 700}, {"Mono", 42, 38, true, 400}};
    if (argc > 2 && std::strcmp(argv[1], "--specimen") == 0) {
        std::vector<Built> fonts;
        for (auto& s : styles) fonts.push_back(build(s));
        std::vector<std::string> lines;
        for (int i = 3; i < argc; i++) lines.push_back(argv[i]);
        specimen(fonts, lines, argv[2]);
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: fontgen <out-dir> | fontgen --specimen <out.pgm> \"<size> <text>\"...\n");
        return 1;
    }
    const char* files[] = {"TributarySans-Regular.ttf", "TributarySans-Bold.ttf", "TributaryMono-Regular.ttf"};
    for (size_t i = 0; i < styles.size(); i++) {
        auto b = build(styles[i]);
        auto data = write_ttf(b.info, b.glyphs);
        std::string path = std::string(argv[1]) + "/" + files[i];
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        std::printf("%s: %zu glyphs, %zu bytes\n", path.c_str(), b.glyphs.size(), data.size());
    }
    return 0;
}
