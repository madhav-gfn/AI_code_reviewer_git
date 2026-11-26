#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace mygit::daemon {

// Default listener port for the local daemon and hosted API.
constexpr int kDefaultPort = 49352;

// How long the daemon can sit idle (no /review or /commit activity) before
// it shuts itself down to free the loaded model's VRAM/RAM.
constexpr int kIdleTimeoutSeconds = 15 * 60;

inline bool hosted_mode() {
    const char* mode = std::getenv("MYGIT_SERVER_MODE");
    return mode && std::string(mode) == "hosted";
}

// Hosted mode uses the platform-provided PORT; local mode uses the explicit
// MYGIT_DAEMON_PORT override. Both fall back to kDefaultPort.
inline int resolve_port() {
    const char* env = hosted_mode() ? std::getenv("PORT") : std::getenv("MYGIT_DAEMON_PORT");
    if (env) {
        try {
            size_t consumed = 0;
            const int port = std::stoi(env, &consumed);
            if (consumed != std::string(env).size() || port < 1 || port > 65535)
                throw std::invalid_argument("port out of range");
            return port;
        } catch (...) {
            throw std::runtime_error("PORT/MYGIT_DAEMON_PORT must be an integer from 1 to 65535");
        }
    }
    return kDefaultPort;
}

}  // namespace mygit::daemon
