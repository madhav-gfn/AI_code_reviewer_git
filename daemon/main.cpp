#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "daemon/config.h"
#include "daemon/server.h"

int main(int argc, char** argv) {
    try {
        const char* mode = std::getenv("MYGIT_SERVER_MODE");
        if (mode && std::string(mode) == "hosted") {
            mygit::daemon::DaemonServer server("", 0, mygit::daemon::resolve_port());
            server.run();
        } else {
            std::string model_path;
            int gpu_layers = 0;
            for (int i = 1; i < argc; ++i) {
                const std::string arg(argv[i]);
                if (arg == "--model-path" && i + 1 < argc) model_path = argv[++i];
                else if (arg == "--gpu-layers" && i + 1 < argc) gpu_layers = std::stoi(argv[++i]);
                else throw std::runtime_error("Usage: mygit_daemon --model-path <path> [--gpu-layers <count>]");
            }
            if (model_path.empty()) throw std::runtime_error("Local daemon requires --model-path; start it with 'mygit daemon start'");
            mygit::daemon::DaemonServer server(model_path, gpu_layers);
            server.run();
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Hosted API startup failed: " << e.what() << "\n";
        return 1;
    }
}
