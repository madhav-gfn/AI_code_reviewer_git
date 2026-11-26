#pragma once

#include <string>

namespace mygit::commands {

// Runs `mygit push <remote> <branch>`.
// Reviews staged/outgoing changes before pushing (FR-3/FR-4). If force_ai is
// true, critical issues are reported but do not block the push.
// Returns a process exit code (0 = success).
int run_push(const std::string& remote, const std::string& branch, bool force_ai);

}  // namespace mygit::commands
