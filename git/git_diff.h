#pragma once

#include <string>

namespace mygit::git {

// Wraps `git` CLI calls (FR-2). V1 shells out directly to the `git`
// executable; a libgit2-backed implementation is planned for V2.
class GitDiff {
public:
    // Returns the diff of currently staged changes (`git diff --staged`).
    // Empty string means there is nothing staged.
    std::string get_staged_diff() const;
};

}  // namespace mygit::git
