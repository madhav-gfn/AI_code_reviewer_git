#include "commands/daemon_command.h"

#include <iostream>
#include <algorithm>
#include <string>

#include "daemon_client.h"
#include "daemon/config.h"

namespace mygit::commands {

int run_daemon(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: mygit daemon <start|stop|status>\n";
        return 1;
    }

    const std::string& action = args.front();
    daemon::DaemonClient client;
    if (action == "start") {
        if (client.is_running()) {
            std::cout << "mygit daemon is already running.\n";
            return 0;
        }
        const bool detached = std::find(args.begin() + 1, args.end(), "-d") != args.end() ||
                              std::find(args.begin() + 1, args.end(), "--detached") != args.end();
        if (!detached) return daemon::run_daemon_foreground();

        daemon::spawn_detached_daemon();
        if (daemon::wait_for_daemon_ready(daemon::resolve_port(), 120)) {
            std::cout << "mygit daemon is ready.\n";
            return 0;
        }
        std::cerr << "Daemon failed to start. Check ~/.mygit/daemon.log.\n";
        return 1;
    }
    if (action == "stop") {
        if (!client.is_running()) {
            std::cout << "mygit daemon is not running.\n";
            return 0;
        }
        if (client.shutdown()) {
            std::cout << "mygit daemon stopped.\n";
            return 0;
        }
        std::cerr << "Failed to stop mygit daemon.\n";
        return 1;
    }
    if (action == "status") {
        if (client.is_running()) {
            std::cout << "mygit daemon is running on port " << daemon::resolve_port() << ".\n";
            return 0;
        }
        std::cout << "mygit daemon is not running.\n";
        return 1;
    }

    std::cerr << "Unknown daemon action: " << action << "\nUsage: mygit daemon <start|stop|status>\n";
    return 1;
}

}  // namespace mygit::commands
