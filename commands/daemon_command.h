#pragma once

#include <string>
#include <vector>

namespace mygit::commands {

// Runs `mygit daemon <start|stop|status>`. `args` is everything after
// "daemon" on the command line (e.g. {"start", "-d"}). Returns a process
// exit code.
int run_daemon(const std::vector<std::string>& args);

}  // namespace mygit::commands
