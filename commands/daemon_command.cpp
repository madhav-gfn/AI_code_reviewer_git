#include "commands/daemon_command.h"

#include <exception>
#include <iostream>

#include "config/mygit_config.h"
#include "daemon/daemon_client.h"
#include "daemon/daemon_config.h"
#include "daemon/daemon_server.h"

namespace mygit::commands {

namespace {

// Runs the daemon in the foreground (blocks for the daemon's whole
// lifetime). Used both by `mygit daemon start` and as the target of the
// detached process spawned by `-d` / DaemonClient::ensure_running().
int run_daemon_foreground() {
    config::MygitConfig cfg;
    try {
        cfg = config::load_config();
    } catch (const std::exception& e) {
        std::cerr << "\n  " << e.what() << "\n\n";
        return 1;
    }

    std::cout << "\n  Loading model into the daemon (this happens once)...\n";
    try {
        daemon::DaemonServer server(cfg.model_path, cfg.gpu_layers);
        std::cout << "  mygit daemon ready on 127.0.0.1:" << daemon::resolve_port() << "\n\n";
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "\n  Failed to start daemon: " << e.what() << "\n\n";
        return 1;
    }
    return 0;
}

int run_daemon_start(bool detached) {
    daemon::DaemonClient probe;
    if (probe.is_running()) {
        std::cout << "\n  mygit daemon is already running.\n\n";
        return 0;
    }

    if (!detached) {
        return run_daemon_foreground();
    }

    std::cout << "\n  Starting mygit daemon in the background...\n";
    daemon::spawn_detached_daemon();
    if (daemon::wait_for_daemon_ready(daemon::resolve_port(), 30)) {
        std::cout << "  mygit daemon is ready.\n\n";
        return 0;
    }
    std::cerr << "\n  Error: Daemon is unreachable and failed to auto-start.\n\n";
    return 1;
}

int run_daemon_stop() {
    daemon::DaemonClient client;
    if (!client.is_running()) {
        std::cout << "\n  mygit daemon is not running.\n\n";
        return 0;
    }
    if (client.shutdown()) {
        std::cout << "\n  mygit daemon stopped.\n\n";
        return 0;
    }
    std::cerr << "\n  Failed to stop the daemon.\n\n";
    return 1;
}

int run_daemon_status() {
    daemon::DaemonClient client;
    if (client.is_running()) {
        std::cout << "\n  mygit daemon is running on port " << daemon::resolve_port() << ".\n\n";
        return 0;
    }
    std::cout << "\n  mygit daemon is not running.\n\n";
    return 1;
}

void print_daemon_usage() {
    std::cout << "\n"
              << "  Usage:\n"
              << "    mygit daemon start [-d]   -- start the daemon (foreground, or -d for background)\n"
              << "    mygit daemon stop         -- stop the running daemon\n"
              << "    mygit daemon status       -- check whether the daemon is running\n"
              << "\n";
}

}  // namespace

int run_daemon(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_daemon_usage();
        return 1;
    }

    const std::string& action = args[0];

    if (action == "start") {
        bool detached = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-d" || args[i] == "--detached") detached = true;
        }
        return run_daemon_start(detached);
    }

    if (action == "stop") return run_daemon_stop();
    if (action == "status") return run_daemon_status();

    std::cerr << "\n  Unknown daemon action: " << action << "\n";
    print_daemon_usage();
    return 1;
}

}  // namespace mygit::commands
