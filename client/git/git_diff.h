#pragma once

#include <string>
#include <vector>

namespace mygit::git {

// Per-file staged diff with rename metadata (bottleneck #4). Preferred over
// the raw patch text from get_staged_diff() when the caller wants to filter
// or batch per file.
struct FileDiff {
    std::string old_path;
    std::string new_path;
    std::string patch;                 // unified diff text for this file
    bool is_rename = false;            // true if this is a rename/move
    bool is_binary = false;            // true if libgit2 flags it as binary
    bool has_content_changes = false;  // true if there are changes beyond the rename itself
};

// Wraps `git` CLI calls (FR-2). V1 shells out directly to the `git`
// executable; a libgit2-backed implementation is planned for V2.
class GitDiff {
public:
    // Returns the diff of currently staged changes (`git diff --staged`).
    // Empty string means there is nothing staged.
    std::string get_staged_diff() const;

    // Returns per-file diffs for currently staged changes, with rename
    // detection applied (git_diff_find_similar). This is the preferred
    // method for the pipeline — it enables per-file batching and lets
    // callers filter out pure renames.
    std::vector<FileDiff> get_staged_file_diffs() const;
};

}  // namespace mygit::git
