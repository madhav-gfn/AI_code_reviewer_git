#pragma once

#include <string>

namespace mygit::commands {

// Runs `mygit commit`: reviews staged changes then invokes `git commit`
// if the review passes. Returns a process exit code.
// message: if non-empty, passed as `git commit -m <message>`.
//          if empty, git opens the user's configured editor as normal.
int run_commit(const std::string& message = {});

}  // namespace mygit::commands
