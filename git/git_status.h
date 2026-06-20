#pragma once

#include <string>

namespace mygit::git {

// Wraps `git` CLI calls for repository/branch state (FR-2).
class GitStatus {
public:
    // Returns the current branch name, e.g. "main".
    std::string get_current_branch() const;

    // True if there are staged changes to review.
    bool has_staged_changes() const;
};

}  // namespace mygit::git
