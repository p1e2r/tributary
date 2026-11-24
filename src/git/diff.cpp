// SPDX-License-Identifier: MIT
#include "diff.hpp"

#include <algorithm>
#include <unordered_map>

namespace git {

bool looks_binary(std::string_view d) {
    size_t n = std::min<size_t>(d.size(), 8000);
    return std::memchr(d.data(), '\0', n) != nullptr;
}

std::string Hunk::header() const {
    return "@@ -" + std::to_string(old_start) + (old_count != 1 ? "," + std::to_string(old_count) : "") +
           " +" + std::to_string(new_start) + (new_count != 1 ? "," + std::to_string(new_count) : "") + " @@";
}

struct Lines {
    std::vector<std::string_view> v;
    bool last_has_newline = true;
};

static Lines to_lines(std::string_view s) {
    Lines l;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) {
            l.v.push_back(s.substr(start));
            l.last_has_newline = false;
            break;
        }
        size_t end = nl;
        l.v.push_back(s.substr(start, end - start));
        start = nl + 1;
    }
    return l;
}

// Myers O(ND) with prefix/suffix trimming, interned lines for fast compares.
std::vector<Edit> myers(const std::vector<std::string_view>& A, const std::vector<std::string_view>& B) {
    std::unordered_map<std::string_view, int> intern;
    std::vector<int> a(A.size()), b(B.size());
    for (size_t i = 0; i < A.size(); i++) a[i] = intern.emplace(A[i], int(intern.size())).first->second;
    for (size_t i = 0; i < B.size(); i++) b[i] = intern.emplace(B[i], int(intern.size())).first->second;

    int n = int(a.size()), m = int(b.size());
    int pre = 0;
    while (pre < n && pre < m && a[pre] == b[pre]) pre++;
    int suf = 0;
    while (suf < n - pre && suf < m - pre && a[n - 1 - suf] == b[m - 1 - suf]) suf++;

    std::vector<Edit> out;
    for (int i = 0; i < pre; i++) out.push_back({Edit::Keep, i, i});

    int N = n - pre - suf, M = m - pre - suf;
    const int* x = a.data() + pre;
    const int* y = b.data() + pre;
    std::vector<Edit> mid;
    if (N == 0) {
        for (int j = 0; j < M; j++) mid.push_back({Edit::Ins, -1, pre + j});
    } else if (M == 0) {
        for (int i = 0; i < N; i++) mid.push_back({Edit::Del, pre + i, -1});
    } else {
        int max = N + M;
        // bound memory for pathological inputs: fall back to delete-all/insert-all
        if (uint64_t(max) * uint64_t(max) > 400'000'000ull && max > 20000) {
            for (int i = 0; i < N; i++) mid.push_back({Edit::Del, pre + i, -1});
            for (int j = 0; j < M; j++) mid.push_back({Edit::Ins, -1, pre + j});
        } else {
            int off = max;
            std::vector<int> v(2 * max + 2, 0);
            std::vector<std::vector<int>> trace;
            int found_d = -1;
            for (int d = 0; d <= max; d++) {
                trace.push_back(std::vector<int>(v.begin() + (off - d), v.begin() + (off + d + 1) + 1));
                for (int k = -d; k <= d; k += 2) {
                    int xx;
                    if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) xx = v[off + k + 1];
                    else xx = v[off + k - 1] + 1;
                    int yy = xx - k;
                    while (xx < N && yy < M && x[xx] == y[yy]) { xx++; yy++; }
                    v[off + k] = xx;
                    if (xx >= N && yy >= M) { found_d = d; break; }
                }
                if (found_d >= 0) break;
            }
            // backtrack
            int cx = N, cy = M;
            std::vector<Edit> rev;
            for (int d = found_d; d > 0; d--) {
                auto& vv = trace[d];  // v snapshot before step d, indices k in [-d, d+1]
                auto get = [&](int k) { return vv[k + d]; };
                int k = cx - cy;
                int prev_k;
                if (k == -d || (k != d && get(k - 1) < get(k + 1))) prev_k = k + 1;
                else prev_k = k - 1;
                int px = get(prev_k);
                int py = px - prev_k;
                while (cx > px && cy > py) { rev.push_back({Edit::Keep, pre + cx - 1, pre + cy - 1}); cx--; cy--; }
                if (cx == px) rev.push_back({Edit::Ins, -1, pre + cy - 1});
                else rev.push_back({Edit::Del, pre + cx - 1, -1});
                cx = px;
                cy = py;
            }
            while (cx > 0 && cy > 0) { rev.push_back({Edit::Keep, pre + cx - 1, pre + cy - 1}); cx--; cy--; }
            std::reverse(rev.begin(), rev.end());
            mid = std::move(rev);
        }
    }
    // Present deletions before insertions inside each change block
    for (size_t i = 0; i < mid.size();) {
        if (mid[i].op == Edit::Keep) { out.push_back(mid[i]); i++; continue; }
        size_t j = i;
        while (j < mid.size() && mid[j].op != Edit::Keep) j++;
        for (size_t k = i; k < j; k++) if (mid[k].op == Edit::Del) out.push_back(mid[k]);
        for (size_t k = i; k < j; k++) if (mid[k].op == Edit::Ins) out.push_back(mid[k]);
        i = j;
    }
    for (int i = 0; i < suf; i++) out.push_back({Edit::Keep, n - suf + i, m - suf + i});
    return out;
}

static std::vector<std::string> strip_ws(const std::vector<std::string_view>& v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (auto l : v) {
        std::string s;
        for (char c : l) if (c != ' ' && c != '\t' && c != '\r') s += c;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<DiffLine> full_diff_lines(std::string_view old_text, std::string_view new_text, bool ignore_ws) {
    auto A = to_lines(old_text), B = to_lines(new_text);
    std::vector<Edit> edits;
    if (ignore_ws) {
        auto an = strip_ws(A.v), bn = strip_ws(B.v);
        std::vector<std::string_view> av(an.begin(), an.end()), bv(bn.begin(), bn.end());
        edits = myers(av, bv);
        // with whitespace ignored, "kept" lines show the new text and never
        // count as changed because of their terminator
        std::vector<DiffLine> out;
        int idx = 0;
        for (auto& e : edits) {
            DiffLine l;
            l.index = idx++;
            if (e.op == Edit::Keep) { l.kind = LineKind::Context; l.old_no = e.a + 1; l.new_no = e.b + 1; l.text = std::string(B.v[e.b]); }
            else if (e.op == Edit::Del) { l.kind = LineKind::Deleted; l.old_no = e.a + 1; l.text = std::string(A.v[e.a]); }
            else { l.kind = LineKind::Added; l.new_no = e.b + 1; l.text = std::string(B.v[e.b]); }
            out.push_back(std::move(l));
        }
        return out;
    }
    edits = myers(A.v, B.v);
    std::vector<DiffLine> out;
    out.reserve(edits.size());
    int idx = 0;
    for (auto& e : edits) {
        DiffLine l;
        l.index = idx++;
        if (e.op == Edit::Keep) {
            l.kind = LineKind::Context;
            l.old_no = e.a + 1;
            l.new_no = e.b + 1;
            l.text = std::string(A.v[e.a]);
            bool a_nonl = !A.last_has_newline && e.a == int(A.v.size()) - 1;
            bool b_nonl = !B.last_has_newline && e.b == int(B.v.size()) - 1;
            if (a_nonl != b_nonl) {
                // the "same" line differs only by its terminator: show as a change
                l.kind = LineKind::Deleted; l.new_no = -1; l.no_newline = a_nonl;
                out.push_back(l);
                DiffLine l2;
                l2.index = idx++;
                l2.kind = LineKind::Added; l2.new_no = e.b + 1; l2.text = std::string(B.v[e.b]);
                l2.no_newline = b_nonl;
                out.push_back(l2);
                continue;
            }
            l.no_newline = a_nonl;
        } else if (e.op == Edit::Del) {
            l.kind = LineKind::Deleted;
            l.old_no = e.a + 1;
            l.text = std::string(A.v[e.a]);
            if (!A.last_has_newline && e.a == int(A.v.size()) - 1) l.no_newline = true;
        } else {
            l.kind = LineKind::Added;
            l.new_no = e.b + 1;
            l.text = std::string(B.v[e.b]);
            if (!B.last_has_newline && e.b == int(B.v.size()) - 1) l.no_newline = true;
        }
        out.push_back(std::move(l));
    }
    return out;
}

FileDiff diff_text(std::string_view old_text, std::string_view new_text, int context, bool ignore_ws) {
    FileDiff fd;
    if (looks_binary(old_text) || looks_binary(new_text)) { fd.binary = true; return fd; }
    if (old_text.size() + new_text.size() > 50 * 1024 * 1024) { fd.too_large = true; return fd; }
    auto lines = full_diff_lines(old_text, new_text, ignore_ws);
    for (auto& l : lines) {
        if (l.kind == LineKind::Added) fd.added++;
        if (l.kind == LineKind::Deleted) fd.deleted++;
    }
    // group into hunks
    int n = int(lines.size());
    int i = 0;
    while (i < n) {
        while (i < n && lines[i].kind == LineKind::Context) i++;
        if (i >= n) break;
        int start = std::max(0, i - context);
        int end = i;
        // extend while changes are within 2*context of each other
        for (;;) {
            while (end < n && lines[end].kind != LineKind::Context) end++;
            int next = end;
            while (next < n && lines[next].kind == LineKind::Context) next++;
            if (next < n && next - end <= 2 * context) { end = next; continue; }
            end = std::min(n, end + context);
            break;
        }
        Hunk h;
        // compute starts
        int os = 0, ns = 0, oc = 0, nc = 0;
        for (int k = start; k < end; k++) {
            auto& l = lines[k];
            if (l.old_no > 0 && !os) os = l.old_no;
            if (l.new_no > 0 && !ns) ns = l.new_no;
            if (l.kind != LineKind::Added) oc++;
            if (l.kind != LineKind::Deleted) nc++;
        }
        if (!os) {  // pure insertion: start is line before
            for (int k = start - 1; k >= 0; k--) if (lines[k].old_no > 0) { os = lines[k].old_no; break; }
            if (oc == 0) os = os; else os = std::max(os, 1);
        }
        if (!ns) {
            for (int k = start - 1; k >= 0; k--) if (lines[k].new_no > 0) { ns = lines[k].new_no; break; }
        }
        h.old_start = os; h.old_count = oc; h.new_start = ns; h.new_count = nc;
        DiffLine hdr;
        hdr.kind = LineKind::HunkHeader;
        hdr.text = h.header();
        hdr.index = -1;
        h.lines.push_back(hdr);
        for (int k = start; k < end; k++) h.lines.push_back(lines[k]);
        fd.hunks.push_back(std::move(h));
        i = end;
    }
    return fd;
}

Bytes apply_selection(std::string_view old_text, std::string_view new_text, const std::vector<bool>& sel) {
    auto lines = full_diff_lines(old_text, new_text);
    Bytes out;
    bool last_nl = true;
    for (auto& l : lines) {
        bool s = l.index < int(sel.size()) ? sel[l.index] : true;
        bool keep = false;
        if (l.kind == LineKind::Context) keep = true;
        else if (l.kind == LineKind::Added) keep = s;
        else if (l.kind == LineKind::Deleted) keep = !s;
        if (!keep) continue;
        out += l.text;
        out += '\n';
        last_nl = !l.no_newline;
    }
    if (!last_nl && !out.empty()) out.pop_back();
    return out;
}

// ---------------------------------------------------------------- merge3
MergeResult merge3(std::string_view base, std::string_view ours, std::string_view theirs,
                   const std::string& ours_label, const std::string& theirs_label) {
    MergeResult r;
    if (ours == theirs) { r.text = Bytes(ours); return r; }
    if (base == ours) { r.text = Bytes(theirs); return r; }
    if (base == theirs) { r.text = Bytes(ours); return r; }
    auto O = to_lines(base), A = to_lines(ours), B = to_lines(theirs);
    auto ea = myers(O.v, A.v), eb = myers(O.v, B.v);
    // map each base line to whether it's kept in A/B, and the inserted chunks
    // Build "chunks": for each base line i, the matched line in A (or -1)
    int n = int(O.v.size());
    std::vector<int> ma(n, -1), mb(n, -1);
    for (auto& e : ea) if (e.op == Edit::Keep) ma[e.a] = e.b;
    for (auto& e : eb) if (e.op == Edit::Keep) mb[e.a] = e.b;
    // Walk sync points: base lines kept in both A and B.
    auto emit = [&](const Lines& L, int from, int to) {
        for (int k = from; k < to; k++) { r.text += L.v[k]; r.text += '\n'; }
    };
    int ia = 0, ib = 0, io = 0;
    auto same_range = [&](const Lines& L1, int a1, int a2, const Lines& L2, int b1, int b2) {
        if (a2 - a1 != b2 - b1) return false;
        for (int k = 0; k < a2 - a1; k++) if (L1.v[a1 + k] != L2.v[b1 + k]) return false;
        return true;
    };
    while (true) {
        // find next sync point at or after io
        int s = io;
        while (s < n && !(ma[s] >= ia && mb[s] >= ib)) s++;
        int ea_end = s < n ? ma[s] : int(A.v.size());
        int eb_end = s < n ? mb[s] : int(B.v.size());
        bool a_changed = !(ea_end - ia == s - io) || [&] {
            for (int k = 0; k < s - io; k++) if (ma[io + k] != ia + k) return true;
            return false;
        }();
        bool b_changed = !(eb_end - ib == s - io) || [&] {
            for (int k = 0; k < s - io; k++) if (mb[io + k] != ib + k) return true;
            return false;
        }();
        if (!a_changed && !b_changed) emit(O, io, s);
        else if (a_changed && !b_changed) emit(A, ia, ea_end);
        else if (!a_changed && b_changed) emit(B, ib, eb_end);
        else if (same_range(A, ia, ea_end, B, ib, eb_end)) emit(A, ia, ea_end);
        else {
            r.conflict = true;
            r.conflicts++;
            r.text += "<<<<<<< " + ours_label + "\n";
            emit(A, ia, ea_end);
            r.text += "=======\n";
            emit(B, ib, eb_end);
            r.text += ">>>>>>> " + theirs_label + "\n";
        }
        if (s >= n) break;
        // emit the sync line itself
        r.text += O.v[s];
        r.text += '\n';
        io = s + 1;
        ia = ma[s] + 1;
        ib = mb[s] + 1;
    }
    // trailing newline handling: follow whichever side changed EOF
    bool nl = A.last_has_newline && B.last_has_newline;
    if (!nl && !r.text.empty() && r.text.back() == '\n' && !r.conflict) r.text.pop_back();
    return r;
}

std::string unified_patch(const std::string& path, const FileDiff& d) {
    std::string s = "--- a/" + path + "\n+++ b/" + path + "\n";
    for (auto& h : d.hunks) {
        for (auto& l : h.lines) {
            switch (l.kind) {
                case LineKind::HunkHeader: s += l.text + "\n"; continue;
                case LineKind::Context: s += " "; break;
                case LineKind::Added: s += "+"; break;
                case LineKind::Deleted: s += "-"; break;
            }
            s += l.text + "\n";
            if (l.no_newline) s += "\\ No newline at end of file\n";
        }
    }
    return s;
}

}  // namespace git
