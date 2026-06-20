#include "git/git_status.h"

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

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

}  // namespace

std::string GitStatus::get_current_branch() const {
    return trim(run_shell_command("git rev-parse --abbrev-ref HEAD"));
}

bool GitStatus::has_staged_changes() const {
    // `git diff --staged --quiet` exits 1 if there are staged differences.
    return run_shell_command("git diff --staged --quiet || echo CHANGED") == "CHANGED\n";
}

}  // namespace mygit::git
