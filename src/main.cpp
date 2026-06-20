#include <iostream>
#include <string>
#include <vector>

#include "commands/push_command.h"
#include "commands/review_command.h"

namespace {

void print_usage() {
    std::cout << "Usage:\n"
              << "  mygit push <remote> <branch> [--force-ai]\n"
              << "  mygit commit\n"
              << "  mygit review\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        print_usage();
        return 1;
    }

    const std::string& command = args[0];

    if (command == "push") {
        bool force_ai = false;
        std::vector<std::string> positional;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--force-ai") {
                force_ai = true;
            } else {
                positional.push_back(args[i]);
            }
        }
        if (positional.size() < 2) {
            std::cerr << "Error: push requires <remote> <branch>\n";
            print_usage();
            return 1;
        }
        return mygit::commands::run_push(positional[0], positional[1], force_ai);
    }

    if (command == "commit") {
        // TODO: FR-8 commit message generation + FR-9 pre-commit hook review.
        std::cout << "mygit commit: not yet implemented\n";
        return 0;
    }

    if (command == "review") {
        return mygit::commands::run_review();
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
