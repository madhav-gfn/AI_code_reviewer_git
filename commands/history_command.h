#pragma once

namespace mygit::commands {

// Runs `mygit history`: prints the last 10 reviews from the SQLite
// database as a formatted table. Returns a process exit code.
int run_history();

}  // namespace mygit::commands
