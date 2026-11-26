#pragma once

namespace mygit::commands {

// Runs `mygit install`: copies the running executable to ~/.mygit/bin/ and
// adds that directory to the user's PATH (Windows registry / POSIX shell rc).
// Returns 0 on success, 1 on failure.
int run_install();

}  // namespace mygit::commands
