#pragma once

#include <string>
#include <vector>

#include "git/git_diff.h"

namespace mygit::diff_filter {

// Tunables for what gets sent to the model. Defaults chosen to keep prompts
// small enough for fast, focused inference (bottleneck #5).
struct DiffFilterConfig {
    size_t max_total_diff_bytes = 50'000;  // reject if total diff exceeds this
    size_t max_per_file_bytes = 20'000;    // skip individual file patches exceeding this
    int max_file_count = 30;               // reject if too many files changed
};

struct FilteredDiff {
    std::string patch_text;            // cleaned unified diff (binary/oversized files removed)
    std::vector<std::string> skipped;  // "<file> (<reason>)" entries for files left out
    bool rejected = false;             // true if the entire diff should not be sent to the model
    std::string reject_reason;

    // Individual files that passed filtering, in order. Only populated by
    // the git::FileDiff overload of filter() — callers that need per-file
    // batching (rather than just the concatenated patch_text) use this.
    std::vector<git::FileDiff> kept_files;
};

// Splits a unified diff (as produced by `git diff --staged`) into per-file
// hunks, drops binary files and hunks exceeding max_per_file_bytes, then
// checks aggregate constraints (total kept size, file count). Returns the
// reassembled patch plus metadata about what was left out or rejected.
FilteredDiff filter(const std::string& raw_diff, const DiffFilterConfig& cfg = {});

// Structured equivalent of filter() that consumes per-file diffs (see
// git::GitDiff::get_staged_file_diffs()). Skips binary files and pure
// renames (renames with no content changes) without re-parsing patch text,
// and only runs the per-file/aggregate patches that remain through the
// same size and count checks as the raw-text overload.
FilteredDiff filter(const std::vector<git::FileDiff>& file_diffs, const DiffFilterConfig& cfg = {});

}  // namespace mygit::diff_filter
