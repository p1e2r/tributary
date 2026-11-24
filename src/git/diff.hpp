// SPDX-License-Identifier: MIT
// Line diffs (Myers), hunks with context, partial-staging helpers and
// three-way text merge with conflict markers.
#pragma once

#include "core.hpp"

namespace git {

enum class LineKind : uint8_t { Context, Added, Deleted, HunkHeader };

struct DiffLine {
    LineKind kind;
    int old_no = -1, new_no = -1;  // 1-based, -1 when absent
    std::string text;              // without trailing newline
    bool no_newline = false;       // "\ No newline at end of file"
    int index = 0;                 // position in the full diff (for selection)
};

struct Hunk {
    int old_start = 0, old_count = 0, new_start = 0, new_count = 0;
    std::vector<DiffLine> lines;   // first line is the header
    std::string header() const;
};

struct FileDiff {
    bool binary = false;
    bool too_large = false;
    std::vector<Hunk> hunks;
    int added = 0, deleted = 0;
};

bool looks_binary(std::string_view data);

// Edit script between two sequences of lines.
struct Edit { enum Op : uint8_t { Keep, Ins, Del } op; int a, b; };
std::vector<Edit> myers(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b);

FileDiff diff_text(std::string_view old_text, std::string_view new_text, int context = 3, bool ignore_ws = false);

// Build the content to stage when only some added/deleted lines (by DiffLine::index)
// are selected. Unselected additions are dropped, unselected deletions kept.
Bytes apply_selection(std::string_view old_text, std::string_view new_text,
                      const std::vector<bool>& selected_by_index);
// Total lines in the full (unhunked) diff, which is the index space used by
// selections.
std::vector<DiffLine> full_diff_lines(std::string_view old_text, std::string_view new_text, bool ignore_ws = false);

struct MergeResult {
    Bytes text;
    bool conflict = false;
    int conflicts = 0;
};
MergeResult merge3(std::string_view base, std::string_view ours, std::string_view theirs,
                   const std::string& ours_label, const std::string& theirs_label);

std::string unified_patch(const std::string& path, const FileDiff& d);

}  // namespace git
