// SPDX-License-Identifier: MIT
// Tributary's icons and illustrations, drawn in code.
//
//   artgen <out-dir>   writes icons.inc (16px line icons as SVG path data) and art/*.png
//
// Icons are skeletons on a 16-unit grid stroked 1.5 units wide with round caps and joins.
// Illustrations are line drawings with light fills, rendered at 2x with our own
// anti-aliased rasterizer and saved as PNG with our own deflate.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../fontgen/geom.hpp"
#include "../../src/git/core.hpp"

using namespace fg;
namespace fs = std::filesystem;

namespace {

// ================================================================ icons
struct Icon {
    std::vector<Contour> c;
    Pen pen{0.75, 0.75};
    void s(Path p, double w = 1.5) { stroke(p, Pen{w / 2, w / 2}, c, {}, 4, true); }
    void line(double x0, double y0, double x1, double y1) { s(Path().M(x0, y0).L(x1, y1)); }
    void poly(std::initializer_list<Pt> pts, bool closed = false) {
        Path p;
        bool first = true;
        for (auto q : pts) {
            if (first) p.M(q.x, q.y);
            else p.L(q.x, q.y);
            first = false;
        }
        if (closed) p.Z();
        s(p);
    }
    void circle(double cx, double cy, double r) { Path p; p.A(cx, cy, r, r, 0, 360, 2.0).Z(); s(p); }
    void arc(double cx, double cy, double r, double a0, double a1) { s(Path().A(cx, cy, r, r, a0, a1, 2.0)); }
    void rrect(double x, double y, double w, double h, double r) {
        Path p;
        p.M(x + r, y).L(x + w - r, y).A(x + w - r, y + r, r, r, -90, 0, 2.0).L(x + w, y + h - r).A(x + w - r, y + h - r, r, r, 0, 90, 2.0)
            .L(x + r, y + h).A(x + r, y + h - r, r, r, 90, 180, 2.0).L(x, y + r).A(x + r, y + r, r, r, 180, 270, 2.0).Z();
        s(p);
    }
    void disc(double cx, double cy, double r) { auto q = superellipse(cx, cy, r, r, 2.0, 32); c.push_back(q); }
    void fillpoly(Contour q) { make_cw(q); c.push_back(q); }
    void node(double cx, double cy) { circle(cx, cy, 1.75); }
};

// The y axis points down (SVG), like the icon renderer expects.
std::map<std::string, std::function<void(Icon&)>> icon_designs() {
    std::map<std::string, std::function<void(Icon&)>> m;
    auto book = [](Icon& i, double x, double y, double w, double h) {
        i.rrect(x, y, w, h, 1.5);
        i.line(x + 2.5, y + 0.2, x + 2.5, y + h - 0.2);   // spine
        i.line(x + 2.5, y + h - 2.8, x + w, y + h - 2.8); // page edge
    };
    m["repo"] = [=](Icon& i) { book(i, 2.75, 1.25, 10.5, 13.5); };
    m["repo-clone"] = [=](Icon& i) {
        book(i, 1.25, 1.25, 8.5, 12);
        i.line(12.5, 6, 12.5, 13.5);
        i.poly({{10.2, 11.4}, {12.5, 13.7}, {14.8, 11.4}});
    };
    m["repo-push"] = [=](Icon& i) {
        book(i, 1.25, 1.25, 8.5, 12);
        i.line(12.5, 14, 12.5, 6.5);
        i.poly({{10.2, 8.6}, {12.5, 6.3}, {14.8, 8.6}});
    };
    m["repo-forked"] = [](Icon& i) {
        i.node(4.5, 2.75); i.node(11.5, 2.75); i.node(8, 13.25);
        i.s(Path().M(4.5, 4.5).L(4.5, 5.6).C(4.5, 7.2, 5.6, 7.8, 8, 7.8).L(8, 11.5));
        i.s(Path().M(11.5, 4.5).L(11.5, 5.6).C(11.5, 7.2, 10.4, 7.8, 8, 7.8));
    };
    m["git-branch"] = [](Icon& i) {
        i.node(4.5, 2.75); i.node(4.5, 13.25); i.node(11.5, 4.25);
        i.line(4.5, 4.5, 4.5, 11.5);
        i.s(Path().M(11.5, 6).C(11.5, 9, 4.5, 8.2, 4.5, 11.2));
    };
    m["git-commit"] = [](Icon& i) { i.circle(8, 8, 2.6); i.line(0.9, 8, 5.4, 8); i.line(10.6, 8, 15.1, 8); };
    m["git-pull-request"] = [](Icon& i) {
        i.node(4, 2.75); i.node(4, 13.25); i.node(12, 13.25);
        i.line(4, 4.5, 4, 11.5);
        i.s(Path().M(12, 11.5).L(12, 6.3).C(12, 4.8, 11.2, 4.2, 9.7, 4.2).L(8.2, 4.2));
        i.poly({{10, 2.3}, {8.1, 4.2}, {10, 6.1}});
    };
    m["git-merge"] = [](Icon& i) {
        i.node(4, 2.75); i.node(4, 13.25); i.node(12, 9);
        i.line(4, 4.5, 4, 11.5);
        i.s(Path().M(4, 4.6).C(4, 7.6, 6.6, 9, 10.25, 9));
    };
    m["git-compare"] = [](Icon& i) {
        i.node(4, 12.75); i.node(12, 3.25);
        i.s(Path().M(4, 11).L(4, 6.4).C(4, 5, 4.8, 4.2, 6.3, 4.2).L(9, 4.2));
        i.poly({{7.2, 2.4}, {9, 4.2}, {7.2, 6}});
        i.s(Path().M(12, 5).L(12, 9.6).C(12, 11, 11.2, 11.8, 9.7, 11.8).L(7, 11.8));
        i.poly({{8.8, 10}, {7, 11.8}, {8.8, 13.6}});
    };
    m["chevron-down"] = [](Icon& i) { i.poly({{4, 6}, {8, 10}, {12, 6}}); };
    m["chevron-up"] = [](Icon& i) { i.poly({{4, 10}, {8, 6}, {12, 10}}); };
    m["chevron-right"] = [](Icon& i) { i.poly({{6, 4}, {10, 8}, {6, 12}}); };
    m["triangle-down"] = [](Icon& i) { i.fillpoly({{4, 6.25}, {12, 6.25}, {8, 10.5}}); };
    m["plus"] = [](Icon& i) { i.line(8, 2.5, 8, 13.5); i.line(2.5, 8, 13.5, 8); };
    m["x"] = [](Icon& i) { i.line(3.5, 3.5, 12.5, 12.5); i.line(12.5, 3.5, 3.5, 12.5); };
    m["check"] = [](Icon& i) { i.poly({{2.5, 8.5}, {6, 12}, {13.5, 4.5}}); };
    m["search"] = [](Icon& i) { i.circle(6.75, 6.75, 4.75); i.line(10.3, 10.3, 14.25, 14.25); };
    m["sync"] = [](Icon& i) {
        i.arc(8, 8, 5.75, 200, 330);
        i.poly({{12.9, 2.2}, {13, 5.2}, {10, 5.3}});
        i.arc(8, 8, 5.75, 20, 150);
        i.poly({{3.1, 13.8}, {3, 10.8}, {6, 10.7}});
    };
    m["arrow-up"] = [](Icon& i) { i.line(8, 14, 8, 2.5); i.poly({{3.5, 7}, {8, 2.5}, {12.5, 7}}); };
    m["arrow-down"] = [](Icon& i) { i.line(8, 2, 8, 13.5); i.poly({{3.5, 9}, {8, 13.5}, {12.5, 9}}); };
    auto doc = [](Icon& i) {
        i.poly({{3.25, 1.25}, {9.5, 1.25}, {12.75, 4.5}, {12.75, 14.75}, {3.25, 14.75}}, true);
        i.poly({{9.5, 1.25}, {9.5, 4.5}, {12.75, 4.5}});
    };
    m["file"] = doc;
    m["file-diff"] = [=](Icon& i) { doc(i); i.line(8, 5.5, 8, 9.5); i.line(6, 7.5, 10, 7.5); i.line(6, 11.75, 10, 11.75); };
    m["file-code"] = [=](Icon& i) { doc(i); i.poly({{6.6, 7}, {5, 9}, {6.6, 11}}); i.poly({{9.4, 7}, {11, 9}, {9.4, 11}}); };
    m["file-directory"] = [](Icon& i) {
        i.s(Path().M(1.25, 4).L(1.25, 3.5).L(1.25, 2.75).L(6, 2.75).L(7.6, 4.6).L(14.75, 4.6).L(14.75, 13.25).L(1.25, 13.25).Z());
    };
    auto sq = [](Icon& i) { i.rrect(1.75, 1.75, 12.5, 12.5, 1.6); };
    m["diff-added"] = [=](Icon& i) { sq(i); i.line(8, 5, 8, 11); i.line(5, 8, 11, 8); };
    m["diff-removed"] = [=](Icon& i) { sq(i); i.line(5, 8, 11, 8); };
    m["diff-modified"] = [=](Icon& i) { sq(i); i.disc(8, 8, 2.1); };
    m["diff-renamed"] = [=](Icon& i) { sq(i); i.line(4.8, 8, 10.8, 8); i.poly({{8.6, 5.6}, {11, 8}, {8.6, 10.4}}); };
    m["lock"] = [](Icon& i) {
        i.s(Path().M(5, 7).L(5, 5).C(5, 3.2, 6.3, 1.75, 8, 1.75).C(9.7, 1.75, 11, 3.2, 11, 5).L(11, 7));
        i.rrect(2.75, 7, 10.5, 7.75, 1.4);
        i.disc(8, 10.9, 1.1);
    };
    m["globe"] = [](Icon& i) {
        i.circle(8, 8, 6.5);
        Path e; e.A(8, 8, 2.7, 6.5, 0, 360, 2.0).Z(); i.s(e);
        i.line(1.6, 8, 14.4, 8);
    };
    m["person"] = [](Icon& i) {
        i.circle(8, 5, 2.9);
        i.s(Path().M(2.5, 14.75).C(2.5, 11, 4.9, 9.4, 8, 9.4).C(11.1, 9.4, 13.5, 11, 13.5, 14.75));
    };
    m["person-add"] = [](Icon& i) {
        i.circle(6, 5, 2.7);
        i.s(Path().M(1.25, 14.75).C(1.25, 11.2, 3.3, 9.5, 6, 9.5).C(8.7, 9.5, 10.75, 11.2, 10.75, 14.75));
        i.line(13, 3, 13, 8); i.line(10.5, 5.5, 15.5, 5.5);
    };
    m["gear"] = [](Icon& i) {
        i.circle(8, 8, 2.2);
        Path p;
        int teeth = 8;
        for (int k = 0; k < teeth; k++) {
            double a = k * 2 * kPi / teeth, w = 0.24;
            auto at = [&](double ang, double r) { return Pt{8 + r * std::cos(ang), 8 + r * std::sin(ang)}; };
            Pt p1 = at(a - w * 1.4, 5.0), p2 = at(a - w, 6.6), p3 = at(a + w, 6.6), p4 = at(a + w * 1.4, 5.0);
            if (k == 0) p.M(p1.x, p1.y); else p.L(p1.x, p1.y);
            p.L(p2.x, p2.y).L(p3.x, p3.y).L(p4.x, p4.y);
        }
        p.Z();
        i.s(p);
    };
    m["three-bars"] = [](Icon& i) { i.line(2, 3.5, 14, 3.5); i.line(2, 8, 14, 8); i.line(2, 12.5, 14, 12.5); };
    m["kebab-horizontal"] = [](Icon& i) { i.disc(3, 8, 1.45); i.disc(8, 8, 1.45); i.disc(13, 8, 1.45); };
    m["clock"] = [](Icon& i) { i.circle(8, 8, 6.5); i.poly({{8, 4.25}, {8, 8}, {10.6, 9.6}}); };
    m["history"] = [](Icon& i) {
        i.arc(8, 8, 6.25, 200, 510);
        i.poly({{1.2, 3.6}, {1.9, 6.1}, {4.4, 5.3}});
        i.poly({{8, 4.5}, {8, 8}, {10.5, 9.5}});
    };
    m["link-external"] = [](Icon& i) {
        i.poly({{6.5, 2.75}, {2.75, 2.75}, {2.75, 13.25}, {13.25, 13.25}, {13.25, 9.5}});
        i.poly({{9.25, 2.75}, {13.25, 2.75}, {13.25, 6.75}});
        i.line(13, 3, 7.5, 8.5);
    };
    m["cloud"] = [](Icon& i) {
        i.s(Path().M(4.6, 12.75).C(2.6, 12.75, 1.25, 11.3, 1.25, 9.6).C(1.25, 7.9, 2.6, 6.6, 4.2, 6.6).C(4.7, 4.4, 6.5, 3, 8.7, 3)
                .C(11.2, 3, 13.1, 4.9, 13.1, 7.3).C(14.2, 7.7, 14.75, 8.8, 14.75, 9.9).C(14.75, 11.5, 13.5, 12.75, 11.9, 12.75).Z());
    };
    auto tray = [](Icon& i) { i.poly({{2, 10}, {2, 14}, {14, 14}, {14, 10}}); };
    m["upload"] = [=](Icon& i) { tray(i); i.line(8, 11, 8, 2.25); i.poly({{4.5, 5.5}, {8, 2}, {11.5, 5.5}}); };
    m["download"] = [=](Icon& i) { tray(i); i.line(8, 1.75, 8, 10.5); i.poly({{4.5, 7.25}, {8, 10.75}, {11.5, 7.25}}); };
    m["alert"] = [](Icon& i) {
        i.poly({{8, 1.9}, {14.9, 13.9}, {1.1, 13.9}}, true);
        i.line(8, 6.2, 8, 9.4);
        i.disc(8, 11.7, 0.95);
    };
    m["info"] = [](Icon& i) { i.circle(8, 8, 6.5); i.disc(8, 4.9, 1.0); i.line(8, 7.5, 8, 11.5); };
    m["question"] = [](Icon& i) {
        i.circle(8, 8, 6.5);
        i.s(Path().M(6, 6.1).C(6, 4.9, 6.9, 4.2, 8, 4.2).C(9.1, 4.2, 10, 4.9, 10, 6).C(10, 7.4, 8, 7.5, 8, 9.1));
        i.disc(8, 11.6, 0.95);
    };
    m["issue-opened"] = [](Icon& i) { i.circle(8, 8, 6.5); i.disc(8, 8, 1.5); };
    m["skip"] = [](Icon& i) { i.circle(8, 8, 6.5); i.line(3.5, 12.5, 12.5, 3.5); };
    m["circle"] = [](Icon& i) { i.circle(8, 8, 6); };
    m["dot-fill"] = [](Icon& i) { i.disc(8, 8, 3.6); };
    m["trash"] = [](Icon& i) {
        i.line(1.75, 4, 14.25, 4);
        i.poly({{5.75, 4}, {5.75, 2}, {10.25, 2}, {10.25, 4}});
        i.poly({{3.5, 4}, {4.4, 14.5}, {11.6, 14.5}, {12.5, 4}});
        i.line(6.6, 7, 6.8, 11.75);
        i.line(9.4, 7, 9.2, 11.75);
    };
    m["pencil"] = [](Icon& i) {
        i.poly({{11.1, 1.6}, {14.4, 4.9}, {5.4, 13.9}, {1.6, 14.4}, {2.1, 10.6}}, true);
        i.line(9.4, 3.3, 12.7, 6.6);
    };
    m["copy"] = [](Icon& i) {
        i.s(Path().M(10.25, 4.5).L(10.25, 2.9).A(8.85, 2.9, 1.4, 1.4, 0, -90, 2.0).L(2.9, 1.5).A(2.9, 2.9, 1.4, 1.4, 270, 180, 2.0)
                .L(1.5, 8.85).A(2.9, 8.85, 1.4, 1.4, 180, 90, 2.0).L(4.5, 10.25));
        i.rrect(5.75, 5.75, 8.75, 8.75, 1.4);
    };
    m["tag"] = [](Icon& i) {
        i.s(Path().M(1.5, 2.75).A(2.75, 2.75, 1.25, 1.25, 180, 270, 2.0).L(8.2, 1.5).L(14.6, 7.9).L(7.9, 14.6).L(1.5, 8.2).Z());
        i.disc(5, 5, 1.2);
    };
    m["stack"] = [](Icon& i) {
        i.poly({{8, 1.5}, {14.5, 5}, {8, 8.5}, {1.5, 5}}, true);
        i.poly({{1.5, 8.25}, {8, 11.75}, {14.5, 8.25}});
        i.poly({{1.5, 11.25}, {8, 14.75}, {14.5, 11.25}});
    };
    m["light-bulb"] = [](Icon& i) {
        i.s(Path().M(5.6, 11.25).C(5.6, 9.4, 3, 8.6, 3, 6).C(3, 3.3, 5.2, 1.5, 8, 1.5).C(10.8, 1.5, 13, 3.3, 13, 6).C(13, 8.6, 10.4, 9.4, 10.4, 11.25).Z());
        i.line(6, 13.25, 10, 13.25);
        i.line(6.8, 15, 9.2, 15);
    };
    m["sign-out"] = [](Icon& i) {
        i.poly({{6.5, 2}, {2.5, 2}, {2.5, 14}, {6.5, 14}});
        i.line(6.5, 8, 14, 8);
        i.poly({{11, 5}, {14, 8}, {11, 11}});
    };
    m["terminal"] = [](Icon& i) {
        i.rrect(1.25, 2.25, 13.5, 11.5, 1.6);
        i.poly({{4.5, 6}, {6.75, 8}, {4.5, 10}});
        i.line(8.5, 10.25, 11.5, 10.25);
    };
    m["code"] = [](Icon& i) { i.poly({{5, 4}, {1, 8}, {5, 12}}); i.poly({{11, 4}, {15, 8}, {11, 12}}); };
    m["eye"] = [](Icon& i) {
        i.s(Path().M(1, 8).C(2.8, 4.7, 5.3, 3, 8, 3).C(10.7, 3, 13.2, 4.7, 15, 8).C(13.2, 11.3, 10.7, 13, 8, 13).C(5.3, 13, 2.8, 11.3, 1, 8).Z());
        i.circle(8, 8, 2.2);
    };
    auto monitor = [](Icon& i) { i.rrect(1.25, 1.75, 13.5, 9.25, 1.4); i.line(8, 11, 8, 14); i.line(5, 14.25, 11, 14.25); };
    m["device-desktop"] = monitor;
    m["desktop-download"] = [=](Icon& i) { monitor(i); i.line(8, 3.8, 8, 8.4); i.poly({{6, 6.6}, {8, 8.6}, {10, 6.6}}); };
    m["undo"] = [](Icon& i) {
        i.s(Path().M(3, 6).L(10, 6).C(12.5, 6, 14.25, 7.8, 14.25, 10).C(14.25, 12.2, 12.5, 14, 10, 14).L(6, 14));
        i.poly({{5.8, 3}, {2.8, 6}, {5.8, 9}});
    };
    m["archive"] = [](Icon& i) {
        i.rrect(1.25, 2.25, 13.5, 3.75, 1.0);
        i.poly({{2.5, 6}, {2.5, 13.75}, {13.5, 13.75}, {13.5, 6}});
        i.line(6.5, 8.75, 9.5, 8.75);
    };
    m["filter"] = [](Icon& i) { i.poly({{1.5, 2.5}, {14.5, 2.5}, {9.5, 8.5}, {9.5, 14}, {6.5, 12.5}, {6.5, 8.5}}, true); };
    auto pin = [](Icon& i) {
        i.line(5, 1.5, 11, 1.5);
        i.poly({{6.25, 1.5}, {6.25, 6}, {3.5, 9.25}, {12.5, 9.25}, {9.75, 6}, {9.75, 1.5}});
        i.line(8, 9.25, 8, 15);
    };
    m["pin"] = pin;
    m["pin-slash"] = [=](Icon& i) { pin(i); i.line(1.5, 1.5, 14.5, 14.5); };
    m["mortar-board"] = [](Icon& i) {
        i.poly({{8, 2.75}, {15, 6.25}, {8, 9.75}, {1, 6.25}}, true);
        i.s(Path().M(4, 8).L(4, 11.2).C(5.5, 12.6, 6.8, 13.2, 8, 13.2).C(9.2, 13.2, 10.5, 12.6, 12, 11.2).L(12, 8));
        i.line(14.5, 6.75, 14.5, 10.75);
    };
    m["sparkle-fill"] = [](Icon& i) {
        Path p;
        p.M(8, 0.75).C(8.5, 5, 11, 7.5, 15.25, 8).C(11, 8.5, 8.5, 11, 8, 15.25).C(7.5, 11, 5, 8.5, 0.75, 8).C(5, 7.5, 7.5, 5, 8, 0.75);
        Contour q = p.p;
        i.fillpoly(q);
    };
    m["tributary-mark"] = [](Icon& i) {  // Tributary's own mark: streams joining into one river
        i.s(Path().M(11.5, 1.5).C(11.5, 5.5, 9, 6.5, 9, 10).L(9, 14.5));
        i.s(Path().M(3.5, 1.5).C(3.5, 6, 9, 5.8, 9, 10));
        i.s(Path().M(2, 9.25).C(5, 9.25, 8, 9.8, 9, 12.5));
    };
    return m;
}

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    std::string s = b;
    while (s.size() > 1 && s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.')) {
        bool dot = s.back() == '.';
        s.pop_back();
        if (dot) break;
    }
    if (s == "-0") s = "0";
    return s;
}

void write_icons(const fs::path& out) {
    g_arc_step = 8;
    std::string src =
        "// SPDX-License-Identifier: MIT\n// Generated by tools/artgen — Tributary's own icon set (16-unit grid).\n#pragma once\n"
        "namespace icons {\nstruct IconDef { const char* name; float size; const char* path; };\ninline const IconDef kIcons[] = {\n";
    for (auto& [name, f] : icon_designs()) {
        Icon ic;
        f(ic);
        std::string d;
        for (auto& c : ic.c) {
            for (size_t k = 0; k < c.size(); k++) d += (k ? "L" : "M") + fmt(c[k].x) + " " + fmt(c[k].y);
            d += "Z";
        }
        src += "    {\"" + name + "\", 16, \"" + d + "\"},\n";
    }
    src += "};\n}  // namespace icons\n";
    std::ofstream(out, std::ios::binary) << src;
}

// ================================================================ illustrations
struct RGBA { double r, g, b, a; };
RGBA hex(uint32_t v, double a = 1) { return {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0, a}; }

struct Canvas {
    int w, h;
    double scale;  // logical → pixels
    std::vector<double> px;  // premultiplied rgba
    Canvas(int w_, int h_, double s) : w(w_), h(h_), scale(s), px(size_t(w_) * h_ * 4, 0) {}

    void fill(const std::vector<Contour>& cs, RGBA col) { fill(cs, [&](double, double) { return col; }); }
    void fill(const std::vector<Contour>& cs, const std::function<RGBA(double, double)>& colf) {
        if (cs.empty()) return;
        double y0 = 1e9, y1 = -1e9, x0 = 1e9, x1 = -1e9;
        for (auto& c : cs) for (auto& p : c) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); }
        int py0 = std::max(0, int(std::floor(y0 * scale))), py1 = std::min(h, int(std::ceil(y1 * scale)) + 1);
        int px0 = std::max(0, int(std::floor(x0 * scale))), px1 = std::min(w, int(std::ceil(x1 * scale)) + 1);
        if (px0 >= px1) return;
        const int SS = 5;
        std::vector<double> cov(size_t(px1 - px0), 0);
        for (int y = py0; y < py1; y++) {
            std::fill(cov.begin(), cov.end(), 0.0);
            for (int sy = 0; sy < SS; sy++) {
                double fy = (y + (sy + 0.5) / SS) / scale;
                std::vector<std::pair<double, int>> xs;
                for (auto& c : cs)
                    for (size_t i = 0; i < c.size(); i++) {
                        Pt a = c[i], b = c[(i + 1) % c.size()];
                        if ((a.y <= fy) == (b.y <= fy)) continue;
                        double t = (fy - a.y) / (b.y - a.y);
                        xs.push_back({(a.x + (b.x - a.x) * t) * scale, b.y > a.y ? 1 : -1});
                    }
                std::sort(xs.begin(), xs.end());
                int wnd = 0;
                for (size_t i = 0; i + 1 < xs.size(); i++) {
                    wnd += xs[i].second;
                    if (!wnd) continue;
                    double a = xs[i].first, b = xs[i + 1].first;
                    // exact horizontal coverage of [a,b] per pixel
                    int ia = std::max(px0, int(std::floor(a))), ib = std::min(px1 - 1, int(std::floor(b)));
                    for (int x = ia; x <= ib; x++) {
                        double l = std::max(a, double(x)), r = std::min(b, double(x + 1));
                        if (r > l) cov[size_t(x - px0)] += (r - l) / SS;
                    }
                }
            }
            for (int x = px0; x < px1; x++) {
                if (cov[size_t(x - px0)] <= 0) continue;
                RGBA col = colf((x + 0.5) / scale, (y + 0.5) / scale);
                double k = std::min(1.0, cov[size_t(x - px0)]) * col.a;
                if (k <= 0) continue;
                double* d = &px[(size_t(y) * w + x) * 4];
                d[0] = col.r * k + d[0] * (1 - k);
                d[1] = col.g * k + d[1] * (1 - k);
                d[2] = col.b * k + d[2] * (1 - k);
                d[3] = k + d[3] * (1 - k);
            }
        }
    }

    std::string png() const {
        auto be32 = [](std::string& s, uint32_t v) { for (int i = 3; i >= 0; i--) s += char((v >> (8 * i)) & 255); };
        auto chunk = [&](std::string& out, const char* type, const std::string& data) {
            be32(out, uint32_t(data.size()));
            std::string td = std::string(type, 4) + data;
            out += td;
            be32(out, git::crc32(td.data(), td.size()));
        };
        std::string raw;
        raw.reserve(size_t(h) * (w * 4 + 1));
        for (int y = 0; y < h; y++) {
            raw += char(0);
            for (int x = 0; x < w; x++) {
                const double* s = &px[(size_t(y) * w + x) * 4];
                double a = s[3];
                auto un = [&](double v) { return char(std::clamp(int(std::lround(a > 0 ? v / a * 255 : 0)), 0, 255)); };
                raw += un(s[0]);
                raw += un(s[1]);
                raw += un(s[2]);
                raw += char(std::clamp(int(std::lround(a * 255)), 0, 255));
            }
        }
        std::string out = "\x89PNG\r\n\x1a\n";
        std::string ihdr;
        be32(ihdr, uint32_t(w));
        be32(ihdr, uint32_t(h));
        ihdr += char(8); ihdr += char(6); ihdr += char(0); ihdr += char(0); ihdr += char(0);
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", git::zlib_deflate(raw, 9));
        chunk(out, "IEND", "");
        return out;
    }
};

// Drawing helpers for illustrations (logical units, y down)
struct Art {
    Canvas& cv;
    RGBA ink = hex(0x9cc3f0), tint = hex(0xe7f1fd), soft = hex(0xd3e6fb);
    double lw = 2.2;
    std::vector<Contour> stroked(const Path& p, double w = -1) {
        std::vector<Contour> out;
        double ww = w < 0 ? lw : w;
        stroke(p, Pen{ww / 2, ww / 2}, out, {}, 4, true);
        return out;
    }
    void s(const Path& p, RGBA c, double w = -1) { cv.fill(stroked(p, w), c); }
    void s(const Path& p) { s(p, ink); }
    void f(const Path& p, RGBA c) { cv.fill({p.p}, c); }
    void shape(const Path& p, RGBA fillc) { f(p, fillc); s(p); }  // filled + outlined
    Path rect(double x, double y, double w, double h, double r) {
        Path p;
        p.M(x + r, y).L(x + w - r, y).A(x + w - r, y + r, r, r, -90, 0, 2.0).L(x + w, y + h - r).A(x + w - r, y + h - r, r, r, 0, 90, 2.0)
            .L(x + r, y + h).A(x + r, y + h - r, r, r, 90, 180, 2.0).L(x, y + r).A(x + r, y + r, r, r, 180, 270, 2.0).Z();
        return p;
    }
    Path ellipse(double cx, double cy, double rx, double ry) { Path p; p.A(cx, cy, rx, ry, 0, 360, 2.0).Z(); return p; }
    void star(double cx, double cy, double r, RGBA c) {  // four-point twinkle
        Path p;
        p.M(cx, cy - r).C(cx + r * 0.12, cy - r * 0.12, cx + r * 0.12, cy - r * 0.12, cx + r, cy).C(cx + r * 0.12, cy + r * 0.12, cx + r * 0.12, cy + r * 0.12, cx, cy + r)
            .C(cx - r * 0.12, cy + r * 0.12, cx - r * 0.12, cy + r * 0.12, cx - r, cy).C(cx - r * 0.12, cy - r * 0.12, cx - r * 0.12, cy - r * 0.12, cx, cy - r);
        f(p, c);
    }
    void dot(double cx, double cy, double r, RGBA c) { f(ellipse(cx, cy, r, r), c); }
    void ring(double cx, double cy, double r, RGBA c, double w = 1.4) { s(ellipse(cx, cy, r, r), c, w); }
    void dashed(const Path& p, double dash, double gap, RGBA c, double w = -1) {
        // walk the flattened polyline and stroke every other stretch
        auto& q = p.p;
        double t = 0;
        bool on = true;
        Path cur;
        cur.M(q[0].x, q[0].y);
        for (size_t i = 1; i < q.size(); i++) {
            Pt a = q[i - 1], b = q[i];
            double L = len(b - a), pos = 0;
            while (pos < L) {
                double lim = (on ? dash : gap) - t, step = std::min(lim, L - pos);
                Pt e = a + (b - a) * ((pos + step) / L);
                if (on) cur.L(e.x, e.y);
                pos += step;
                t += step;
                if (t >= (on ? dash : gap) - 1e-9) {
                    if (on && cur.p.size() > 1) s(cur, c, w);
                    on = !on;
                    t = 0;
                    cur = Path();
                    cur.M(e.x, e.y);
                }
            }
        }
        if (on && cur.p.size() > 1) s(cur, c, w);
    }
    // a page with text lines, optionally rotated about its centre
    void page(double x, double y, double w, double h, double deg, int lines, RGBA fillc) {
        double a = deg * kPi / 180, cx = x + w / 2, cy = y + h / 2;
        auto rot = [&](Path p) {
            for (auto& q : p.p) {
                double dx = q.x - cx, dy = q.y - cy;
                q = {cx + dx * std::cos(a) - dy * std::sin(a), cy + dx * std::sin(a) + dy * std::cos(a)};
            }
            return p;
        };
        double fold = w * 0.22;
        Path body;
        body.M(x, y + 3).A(x + 3, y + 3, 3, 3, 180, 270, 2.0).L(x + w - fold, y).L(x + w, y + fold).L(x + w, y + h - 3)
            .A(x + w - 3, y + h - 3, 3, 3, 0, 90, 2.0).L(x + 3, y + h).A(x + 3, y + h - 3, 3, 3, 90, 180, 2.0).Z();
        body = rot(body);
        f(body, fillc);
        s(body);
        s(rot(Path().M(x + w - fold, y).L(x + w - fold, y + fold).L(x + w, y + fold)));
        for (int i = 0; i < lines; i++) {
            double ly = y + h * 0.3 + i * h * 0.14, lw2 = (i % 3 == 2 ? 0.45 : 0.68) * w;
            s(rot(Path().M(x + w * 0.16, ly).L(x + w * 0.16 + lw2, ly)), soft, 2.0);
        }
    }
    void cursor(double x, double y, double sz) {
        Path p;
        p.M(x, y).L(x, y + sz).L(x + sz * 0.28, y + sz * 0.74).L(x + sz * 0.46, y + sz * 1.08).L(x + sz * 0.6, y + sz * 1.0).L(x + sz * 0.42, y + sz * 0.68)
            .L(x + sz * 0.78, y + sz * 0.66).Z();
        f(p, hex(0xffffff));
        s(p);
    }
    void ground(double cx, double cy, double rx) { f(ellipse(cx, cy, rx, rx * 0.09), soft); }
};

using ArtFn = std::function<void(Art&)>;
struct ArtDef { const char* name; double w, h; ArtFn draw; };

std::vector<ArtDef> art_designs() {
    std::vector<ArtDef> v;
    // "No local changes": a small stack of finished pages with a check badge
    v.push_back({"paper-stack", 73, 70, [](Art& a) {
        a.lw = 1.6;
        a.page(16, 16, 36, 46, 8, 0, a.tint);
        a.page(10, 12, 36, 46, -4, 4, hex(0xffffff));
        a.dot(54, 17, 11, a.tint);
        a.ring(54, 17, 11, a.ink, 1.6);
        a.s(Path().M(49, 17.5).L(52.6, 21).L(59, 13.5), a.ink, 1.8);
        a.star(66, 44, 3.2, a.soft);
        a.star(6, 60, 2.6, a.soft);
    }});
    // No file selected: a page and a pointer looking for something to open
    v.push_back({"empty-no-file-selected", 288, 147, [](Art& a) {
        a.ground(144, 132, 92);
        a.page(96, 34, 66, 88, -9, 0, a.tint);
        a.page(104, 28, 66, 88, 0, 5, hex(0xffffff));
        a.cursor(176, 66, 34);
        a.dashed(Path().M(220, 40).C(206, 50, 196, 56, 188, 68), 4, 5, a.soft, 2);
        a.star(232, 34, 6, a.soft);
        a.star(60, 52, 5, a.soft);
        a.dot(78, 96, 2.4, a.soft);
        a.dot(222, 104, 2, a.soft);
        a.ring(46, 104, 4.5, a.soft, 1.6);
        a.star(250, 86, 3.4, a.soft);
    }});
    // Several files selected: a fan of pages
    v.push_back({"multiple-files-selected", 288, 147, [](Art& a) {
        a.ground(144, 132, 100);
        a.page(84, 34, 62, 84, -16, 4, a.tint);
        a.page(142, 34, 62, 84, 16, 4, a.tint);
        a.page(113, 24, 62, 88, 0, 5, hex(0xffffff));
        a.star(64, 40, 5, a.soft);
        a.star(228, 46, 6, a.soft);
        a.dot(240, 98, 2.2, a.soft);
        a.dot(50, 100, 2.4, a.soft);
        a.ring(236, 22, 4, a.soft, 1.6);
    }});
    // No commit selected: a history line, its last commit still to come
    v.push_back({"empty-no-commit", 288, 138, [](Art& a) {
        double y = 74;
        a.s(Path().M(34, y).L(170, y), a.ink, 2.4);
        a.dashed(Path().M(170, y).L(236, y), 6, 6, a.soft, 2.4);
        a.s(Path().M(98, y).C(116, y, 120, y - 36, 140, y - 36).L(160, y - 36), a.ink, 2.4);
        for (double x : {50.0, 98.0, 150.0}) { a.dot(x, y, 9, hex(0xffffff)); a.ring(x, y, 9, a.ink, 2.4); }
        a.dot(170, y - 36, 9, hex(0xffffff));
        a.ring(170, y - 36, 9, a.ink, 2.4);
        a.dashed(a.ellipse(246, y, 10, 10), 4, 4, a.ink, 2.2);
        a.s(Path().M(246, y - 4.5).L(246, y + 4.5), a.soft, 2);
        a.s(Path().M(241.5, y).L(250.5, y), a.soft, 2);
        a.star(214, 34, 6, a.soft);
        a.star(60, 30, 4.5, a.soft);
        a.dot(200, 112, 2.2, a.soft);
        a.dot(120, 116, 2.6, a.soft);
        a.ring(30, 112, 4, a.soft, 1.6);
    }});
    // Welcome, top right: a ringed planet
    v.push_back({"welcome-illustration-left-top", 42.3, 43.8, [](Art& a) {
        RGBA line = hex(0x9cc3f0, 0.9), fillc = hex(0xe7f1fd, 0.9);
        a.lw = 1.4;
        // the ring's far half goes behind the planet, the near half in front
        auto ring = [&](double a0, double a1) {
            Path r;
            r.A(21, 22, 20, 6, a0, a1, 2.0);
            for (auto& q : r.p) {  // tilt
                double dx = q.x - 21, dy = q.y - 22, t = -0.38;
                q = {21 + dx * std::cos(t) - dy * std::sin(t), 22 + dx * std::sin(t) + dy * std::cos(t)};
            }
            return r;
        };
        a.s(ring(180, 360), line, 1.4);
        a.f(a.ellipse(21, 22, 13, 13), fillc);
        a.s(a.ellipse(21, 22, 13, 13), line, 1.4);
        a.s(ring(0, 180), line, 1.4);
        a.f(a.ellipse(17, 18, 2.4, 2.4), hex(0xd3e6fb));
        a.f(a.ellipse(25.5, 26, 1.7, 1.7), hex(0xd3e6fb));
        a.star(37, 6, 2.4, hex(0xb7d4f5));
        a.dot(5, 38, 1.0, hex(0xb7d4f5));
    }});
    // Welcome, bottom right: a paper plane setting off past a few stars
    v.push_back({"welcome-illustration-left-bottom", 114, 172.3, [](Art& a) {
        RGBA line = hex(0x9cc3f0, 0.9), fillc = hex(0xe7f1fd, 0.95), dim = hex(0xc4dcf7, 0.9);
        a.lw = 1.5;
        Path plane;
        plane.M(92, 34).L(46, 54).L(62, 60).L(66, 78).L(74, 64).L(88, 70).Z();
        a.f(plane, fillc);
        a.s(plane, line, 1.5);
        a.s(Path().M(62, 60).L(92, 34).L(74, 64), line, 1.3);
        a.dashed(Path().M(60, 76).C(40, 96, 66, 112, 52, 132).C(42, 146, 26, 150, 14, 164), 4, 5, dim, 1.4);
        a.star(24, 30, 4, dim);
        a.star(100, 104, 3.2, dim);
        a.star(88, 150, 4.6, dim);
        a.dot(14, 80, 1.3, dim);
        a.dot(104, 16, 1.2, dim);
        a.ring(34, 112, 2.8, dim, 1.2);
        a.dot(80, 128, 1.1, dim);
    }});
    return v;
}

// The application icon: Tributary's mark on a blue-violet tile.
void write_app_icon(const fs::path& dir) {
    fs::create_directories(dir);
    auto render = [](int size) {
        double sc = size / 1024.0;
        Canvas cv(size, size, sc);
        Art a{cv};
        Path tile;
        tile.A(512, 512, 412, 412, 0, 360, 5.0).Z();  // squircle
        RGBA c0 = hex(0x2f81f7), c1 = hex(0x8250df);
        cv.fill({tile.p}, [&](double x, double y) {
            double t = std::clamp((x + y - 200) / 1648.0, 0.0, 1.0);
            return RGBA{c0.r + (c1.r - c0.r) * t, c0.g + (c1.g - c0.g) * t, c0.b + (c1.b - c0.b) * t, 1};
        });
        // the mark (same drawing as the 16px icon, scaled up)
        auto T = [](double x, double y) { return Pt{212 + x * 37.5, 212 + y * 37.5}; };
        auto P = [&](std::vector<std::array<double, 6>> segs, Pt start) {
            Path p;
            Pt s0 = T(start.x, start.y);
            p.M(s0.x, s0.y);
            for (auto& c : segs) {
                if (c[2] > 1e8) { Pt q = T(c[0], c[1]); p.L(q.x, q.y); }
                else { Pt q1 = T(c[0], c[1]), q2 = T(c[2], c[3]), q3 = T(c[4], c[5]); p.C(q1.x, q1.y, q2.x, q2.y, q3.x, q3.y); }
            }
            return p;
        };
        const double L = 1e9;
        RGBA white = hex(0xffffff);
        double w = 1.7 * 37.5;
        a.s(P({{11.5, 5.5, 9, 6.5, 9, 10}, {9, 14.5, L, 0, 0, 0}}, {11.5, 1.5}), white, w);
        a.s(P({{3.5, 6, 9, 5.8, 9, 10}}, {3.5, 1.5}), white, w);
        a.s(P({{5, 9.25, 8, 9.8, 9, 12.5}}, {2, 9.25}), white, w);
        return cv.png();
    };
    std::map<int, std::string> png;
    for (int s : {16, 32, 48, 128, 256, 512, 1024}) png[s] = render(s);
    std::ofstream(dir / "icon-1024.png", std::ios::binary) << png[1024];
    std::ofstream(dir / "icon-256.png", std::ios::binary) << png[256];
    auto le16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char((v >> 8) & 255); };
    auto le32 = [&](std::string& o, uint32_t v) { le16(o, v & 0xFFFF); le16(o, v >> 16); };
    auto be32 = [](std::string& o, uint32_t v) { for (int i = 3; i >= 0; i--) o += char((v >> (8 * i)) & 255); };
    {  // .ico with PNG entries
        std::vector<int> sizes = {16, 32, 48, 256};
        std::string ico;
        le16(ico, 0); le16(ico, 1); le16(ico, uint32_t(sizes.size()));
        uint32_t off = 6 + 16 * uint32_t(sizes.size());
        for (int sz : sizes) {
            ico += char(sz >= 256 ? 0 : sz); ico += char(sz >= 256 ? 0 : sz); ico += char(0); ico += char(0);
            le16(ico, 1); le16(ico, 32); le32(ico, uint32_t(png[sz].size())); le32(ico, off);
            off += uint32_t(png[sz].size());
        }
        for (int sz : sizes) ico += png[sz];
        std::ofstream(dir / "Tributary.ico", std::ios::binary) << ico;
    }
    {  // .icns with PNG entries
        std::vector<std::pair<const char*, int>> ent = {{"icp4", 16}, {"icp5", 32}, {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}};
        std::string body;
        for (auto& [t, sz] : ent) { body += t; be32(body, uint32_t(8 + png[sz].size())); body += png[sz]; }
        std::string icns = "icns";
        be32(icns, uint32_t(8 + body.size()));
        icns += body;
        std::ofstream(dir / "Tributary.icns", std::ios::binary) << icns;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--app-icon") {
        write_app_icon(argv[2]);
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: artgen <out-dir>\n");
        return 1;
    }
    fs::path out = argv[1];
    fs::create_directories(out / "art");
    write_icons(out / "icons.inc");
    g_arc_step = 3;
    for (auto& d : art_designs()) {
        double sc = 2;  // retina
        if (std::string(d.name).rfind("welcome", 0) == 0) sc = 4;  // shown up to 88/344px tall
        Canvas cv(int(std::ceil(d.w * sc)), int(std::ceil(d.h * sc)), sc);
        Art a{cv};
        d.draw(a);
        std::ofstream(out / "art" / (std::string(d.name) + ".png"), std::ios::binary) << cv.png();
    }
    std::printf("artgen: icons + %zu illustrations -> %s\n", art_designs().size(), out.string().c_str());
    return 0;
}
