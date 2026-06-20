#pragma once

namespace mygit::commands {

// Runs `mygit review`: reviews staged changes and prints a report without
// performing any git operation. Returns a process exit code.
int run_review();

}  // namespace mygit::commands
