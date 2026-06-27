#include "git/git_runner.h"

#include <cstdlib>

namespace mygit::git {

int run_git(const std::string& args) {
    // std::system() on Windows runs through cmd.exe /c, on POSIX through
    // /bin/sh -c. In both cases git's stdout/stderr flow to the terminal
    // unchanged, which is exactly what we want - the user sees git's own
    // progress output (remote counts, pack stats, rejection messages, etc.)
    // without us having to capture and re-print it.
    return std::system(("git " + args).c_str());
}

}  // namespace mygit::git
