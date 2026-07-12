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

std::string run_git_capture(const std::string& args) {
    const std::string cmd = "git " + args;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return {};

    std::string result;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }

#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    // Trim trailing whitespace / newlines.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

}  // namespace mygit::git
