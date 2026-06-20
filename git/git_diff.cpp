#include "git/git_diff.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace mygit::git {

namespace {

std::string run_shell_command(const std::string& command) {
    std::array<char, 4096> buffer{};
    std::string result;

#if defined(_WIN32)
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(command.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
#endif

    if (!pipe) {
        throw std::runtime_error("Failed to run command: " + command);
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

}  // namespace

std::string GitDiff::get_staged_diff() const {
    return run_shell_command("git diff --staged");
}

}  // namespace mygit::git
