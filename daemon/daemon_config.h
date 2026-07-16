#pragma once

#include <cstdlib>
#include <string>

namespace mygit::daemon {

// Localhost port the daemon listens on, unless overridden.
constexpr int kDefaultPort = 49152;

// How long the daemon can sit idle (no /review or /commit activity) before
// it shuts itself down to free the loaded model's VRAM/RAM.
constexpr int kIdleTimeoutSeconds = 15 * 60;

// Resolves the daemon's localhost port from the MYGIT_DAEMON_PORT env var,
// falling back to kDefaultPort. Mirrors the env-override pattern used for
// the model path/GPU layers in ai/llama_client.cpp.
inline int resolve_port() {
    if (const char* env = std::getenv("MYGIT_DAEMON_PORT")) {
        try {
            return std::stoi(env);
        } catch (...) {
            // fall through to default
        }
    }
    return kDefaultPort;
}

}  // namespace mygit::daemon
