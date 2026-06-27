#pragma once

#include <string>

namespace mygit::git {

// Executes `git <args>` as a subprocess. stdout/stderr pass straight
// through to the terminal so the user sees git's own output (progress
// bars, error messages, etc.) exactly as if they'd run git directly.
// Returns git's exit code: 0 = success, non-zero = failure.
int run_git(const std::string& args);

}  // namespace mygit::git
