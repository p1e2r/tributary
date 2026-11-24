// SPDX-License-Identifier: MIT
// Geometry for Tributary's font generator: skeleton paths are flattened to polylines and
// stroked with an elliptical pen into overlapping clockwise contours (filled non-zero).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace fg {

constexpr double kPi = 3.14159265358979323846;
inline double g_arc_step = 3.0;  // degrees per segment when flattening arcs

struct Pt {
    double x = 0, y = 0;
    Pt operator+(Pt o) const { return {x + o.x, y + o.y}; }
    Pt operator-(Pt o) const { return {x - o.x, y - o.y}; }
    Pt operator*(double s) const { return {x * s, y * s}; }
};
inline double dot(Pt a, Pt b) { return a.x * b.x + a.y * b.y; }
inline double cross(Pt a, Pt b) { return a.x * b.y - a.y * b.x; }
inline double len(Pt a) { return std::sqrt(dot(a, a)); }
inline Pt norm(Pt a) { double l = len(a); return l > 1e-12 ? a * (1 / l) : Pt{0, 0}; }

using Contour = std::vector<Pt>;

inline double area(const Contour& c) {
    double a = 0;
    for (size_t i = 0; i < c.size(); i++) a += cross(c[i], c[(i + 1) % c.size()]);
    return a / 2;
}
// TrueType: outer contours clockwise (y up). Everything we emit is an outer contour.
inline void make_cw(Contour& c) { if (area(c) > 0) std::reverse(c.begin(), c.end()); }

struct Clip {  // keep x0<=x<=x1, y0<=y<=y1
    double x0 = -1e9, y0 = -1e9, x1 = 1e9, y1 = 1e9;
    bool any() const { return x0 > -1e8 || y0 > -1e8 || x1 < 1e8 || y1 < 1e8; }
};

inline Contour clip_half(const Contour& in, int axis, double v, bool keep_greater) {
    Contour out;
    auto inside = [&](Pt p) { double c = axis ? p.y : p.x; return keep_greater ? c >= v : c <= v; };
    for (size_t i = 0; i < in.size(); i++) {
        Pt a = in[i], b = in[(i + 1) % in.size()];
        bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            double ca = axis ? a.y : a.x, cb = axis ? b.y : b.x;
            double t = (v - ca) / (cb - ca);
            out.push_back(a + (b - a) * t);
        }
    }
    return out;
}
inline Contour clip(const Contour& c, const Clip& k) {
    Contour r = c;
    if (k.x0 > -1e8) r = clip_half(r, 0, k.x0, true);
    if (r.size() > 2 && k.x1 < 1e8) r = clip_half(r, 0, k.x1, false);
    if (r.size() > 2 && k.y0 > -1e8) r = clip_half(r, 1, k.y0, true);
    if (r.size() > 2 && k.y1 < 1e8) r = clip_half(r, 1, k.y1, false);
    return r;
}

// A skeleton path. Curves are flattened as they're added; `corner` marks vertices where the
// direction changes abruptly (those get a mitred join).
struct Path {
    std::vector<Pt> p;
    std::vector<char> corner;
    bool closed = false;
    double superness = 2.25;

    Path& M(double x, double y) { p = {{x, y}}; corner = {0}; return *this; }
    void push(Pt q) {
        if (!p.empty() && len(q - p.back()) < 0.05) return;
        if (p.size() >= 2) {
            Pt d1 = norm(p.back() - p[p.size() - 2]), d2 = norm(q - p.back());
            if (dot(d1, d2) < std::cos(9 * kPi / 180) && !pending_smooth) corner.back() = 1;
        }
        pending_smooth = false;
        p.push_back(q);
        corner.push_back(0);
    }
    bool pending_smooth = false;
    Path& L(double x, double y) { push({x, y}); return *this; }
    Path& C(double x1, double y1, double x2, double y2, double x, double y) {
        Pt p0 = p.back(), p1{x1, y1}, p2{x2, y2}, p3{x, y};
        double l = len(p1 - p0) + len(p2 - p1) + len(p3 - p2);
        int n = std::clamp(int(l / 9), 6, 96);
        for (int i = 1; i <= n; i++) {
            double t = double(i) / n, u = 1 - t;
            push(p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t));
        }
        return *this;
    }
    // superellipse arc centred (cx,cy) from angle a0 to a1 (degrees, CCW positive)
    Path& A(double cx, double cy, double rx, double ry, double a0, double a1, double n = 0) {
        if (n <= 0) n = superness;
        int steps = std::max(4, int(std::fabs(a1 - a0) / g_arc_step));
        bool first = p.empty();
        for (int i = first ? 0 : 0; i <= steps; i++) {
            double a = (a0 + (a1 - a0) * i / steps) * kPi / 180;
            double c = std::cos(a), s = std::sin(a);
            double x = cx + rx * (c < 0 ? -1 : 1) * std::pow(std::fabs(c), 2 / n);
            double y = cy + ry * (s < 0 ? -1 : 1) * std::pow(std::fabs(s), 2 / n);
            if (p.empty()) { p = {{x, y}}; corner = {0}; }
            else push({x, y});
        }
        return *this;
    }
    Path& Z() {
        if (p.size() > 2 && len(p.back() - p.front()) < 0.5) { p.pop_back(); corner.pop_back(); }
        closed = true;
        if (p.size() > 2) {
            Pt d1 = norm(p.front() - p.back()), d2 = norm(p[1] - p[0]);
            if (dot(d1, d2) < std::cos(9 * kPi / 180)) corner[0] = 1;
        }
        return *this;
    }
};

struct Pen {
    double hx, hy;  // half thickness of vertical / horizontal strokes
    Pt support(Pt n) const {  // pen offset for a unit normal
        double d = std::sqrt(hx * hx * n.x * n.x + hy * hy * n.y * n.y);
        if (d < 1e-12) return {0, 0};
        return {hx * hx * n.x / d, hy * hy * n.y / d};
    }
};

inline Pt left_normal(Pt d) { return {-d.y, d.x}; }

inline Contour superellipse(double cx, double cy, double rx, double ry, double sn = 2.6, int steps = 48);

// round = true: round caps and joins (icon line-work) instead of butt caps and mitres
inline void stroke(const Path& path, const Pen& pen, std::vector<Contour>& out, const Clip& clipr = {}, double miter_limit = 4, bool round = false) {
    std::vector<Pt> p = path.p;
    std::vector<char> corner = path.corner;
    size_t n = p.size();
    if (n < 2) return;
    auto emit = [&](Contour c) {
        if (clipr.any()) c = clip(c, clipr);
        if (c.size() < 3 || std::fabs(area(c)) < 1e-4) return;
        make_cw(c);
        out.push_back(std::move(c));
    };
    // closed: duplicate the first point at the end so runs can wrap
    if (path.closed) { p.push_back(p[0]); corner.push_back(corner[0]); n = p.size(); }
    std::vector<Pt> dir(n - 1);
    for (size_t i = 0; i + 1 < n; i++) dir[i] = norm(p[i + 1] - p[i]);
    // break into runs at corners and every ~80 degrees of turning
    std::vector<size_t> breaks = {0};
    double turn = 0;
    for (size_t i = 1; i + 1 < n; i++) {
        double a = std::acos(std::clamp(dot(dir[i - 1], dir[i]), -1.0, 1.0));
        turn += a;
        if (corner[i] || turn > 80 * kPi / 180) { breaks.push_back(i); turn = 0; }
    }
    breaks.push_back(n - 1);
    for (size_t b = 0; b + 1 < breaks.size(); b++) {
        size_t i0 = breaks[b], i1 = breaks[b + 1];
        if (i1 <= i0) continue;
        auto smooth_off = [&](Pt d1, Pt d2) {
            Pt n1 = left_normal(d1), n2 = left_normal(d2);
            Pt na = norm(n1 + n2);
            double c = std::max(0.3, dot(na, n1));
            return pen.support(na) * (1 / c);
        };
        std::vector<Pt> off;
        for (size_t k = i0; k <= i1; k++) {
            Pt o;
            bool is_corner = corner[k] || (path.closed && k == n - 1 && corner[0]);
            if (k == i0) {
                // a smooth break shares the averaged offset with the previous run (no seam)
                if (!is_corner && k > 0) o = smooth_off(dir[k - 1], dir[k]);
                else if (!is_corner && k == 0 && path.closed) o = smooth_off(dir[n - 2], dir[0]);
                else o = pen.support(left_normal(dir[k]));
            } else if (k == i1) {
                if (!is_corner && k + 1 < n) o = smooth_off(dir[k - 1], dir[k]);
                else if (!is_corner && k == n - 1 && path.closed) o = smooth_off(dir[k - 1], dir[0]);
                else o = pen.support(left_normal(dir[k - 1]));
            } else {
                o = smooth_off(dir[k - 1], dir[k]);
            }
            off.push_back(o);
        }
        Contour c;
        for (size_t k = i0; k <= i1; k++) c.push_back(p[k] + off[k - i0]);
        for (size_t k = i1 + 1; k-- > i0;) c.push_back(p[k] - off[k - i0]);
        emit(c);
    }
    // mitred joins at corners
    auto join = [&](size_t vi, Pt d1, Pt d2) {
        Pt v = p[vi];
        double cr = cross(d1, d2);
        if (std::fabs(cr) < 1e-6 && dot(d1, d2) > 0) return;
        double s = cr > 0 ? -1 : 1;  // outer side
        Pt a = v + pen.support(left_normal(d1)) * s, bb = v + pen.support(left_normal(d2)) * s;
        // intersect a + t d1 with bb - u d2
        double den = cross(d1, d2 * -1);
        Contour w;
        if (std::fabs(den) > 1e-9) {
            double t = cross(bb - a, d2 * -1) / den;
            Pt m = a + d1 * t;
            if (len(m - v) <= miter_limit * std::max(pen.hx, pen.hy) && t > -1e-6) w = {v, a, m, bb};
            else w = {v, a, bb};
        } else {
            w = {v, a, bb};
        }
        emit(w);
    };
    if (round) {
        auto disc = [&](Pt c) { emit(superellipse(c.x, c.y, pen.hx, pen.hy, 2.0, 16)); };
        for (size_t i = 1; i + 1 < n; i++)
            if (corner[i]) disc(p[i]);
        if (path.closed) { if (corner[0]) disc(p[0]); }
        else { disc(p[0]); disc(p[n - 1]); }
        return;
    }
    for (size_t i = 1; i + 1 < n; i++)
        if (corner[i]) join(i, dir[i - 1], dir[i]);
    if (path.closed && corner[0]) join(0, dir[n - 2], dir[0]);
}

inline Contour superellipse(double cx, double cy, double rx, double ry, double sn, int steps) {
    Contour c;
    for (int i = 0; i < steps; i++) {
        double a = 2 * kPi * i / steps, co = std::cos(a), si = std::sin(a);
        c.push_back({cx + rx * (co < 0 ? -1 : 1) * std::pow(std::fabs(co), 2 / sn), cy + ry * (si < 0 ? -1 : 1) * std::pow(std::fabs(si), 2 / sn)});
    }
    make_cw(c);
    return c;
}

}  // namespace fg
