#include <iostream>
#include <string>
#include <vector>

#include "commands/commit_command.h"
#include "commands/history_command.h"
#include "commands/install_command.h"
#include "commands/push_command.h"
#include "commands/review_command.h"
#include "commands/setup_command.h"
#include "ui/terminal_ui.h"

namespace {

void print_usage() {
    std::cout << "\n"
              << "  Usage:\n"
              << "    mygit setup                         -- configure model path\n"
              << "    mygit install                       -- install to PATH\n"
              << "    mygit review                        -- review staged changes\n"
              << "    mygit commit [-m \"message\"]         -- review then commit\n"
              << "    mygit push <remote> <branch>        -- review then push\n"
              << "    mygit push <remote> <branch> --force-ai\n"
              << "    mygit history                       -- show last 10 reviews\n"
              << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    mygit::ui::enable_colors();

    const std::vector<std::string> args(argv + 1, argv + argc);

    try {
        if (args.empty()) {
            print_usage();
            return 1;
        }

        const std::string& command = args[0];

        if (command == "setup") {
            return mygit::commands::run_setup();
        }

        if (command == "install") {
            return mygit::commands::run_install();
        }

        if (command == "review") {
            return mygit::commands::run_review();
        }

        if (command == "commit") {
            std::string message;
            for (size_t i = 1; i + 1 < args.size(); ++i) {
                if (args[i] == "-m") {
                    message = args[i + 1];
                    break;
                }
            }
            return mygit::commands::run_commit(message);
        }

        if (command == "push") {
            bool force_ai = false;
            std::vector<std::string> positional;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--force-ai") force_ai = true;
                else positional.push_back(args[i]);
            }
            if (positional.size() < 2) {
                std::cerr << "\n  Error: push requires <remote> <branch>\n";
                print_usage();
                return 1;
            }
            return mygit::commands::run_push(positional[0], positional[1], force_ai);
        }

        if (command == "history") {
            return mygit::commands::run_history();
        }

        std::cerr << "\n  Unknown command: " << command << "\n";
        print_usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\n  Fatal Error: " << e.what() << "\n\n";
        return 1;
    } catch (...) {
        std::cerr << "\n  Fatal Error: Unknown exception occurred.\n\n";
        return 1;
    }
}
