#pragma once

#include <memory>
#include <string>

#include "daemon/daemon_config.h"

namespace mygit::daemon {

// Hosts a persistent ai::LlamaClient behind a localhost HTTP server so the
// model is loaded once and reused across every `mygit review`/`mygit commit`
// invocation instead of being loaded fresh by each CLI process (Verdict #2 -
// zero cold starts, see docs/architecture_review.md).
//
// httplib.h is intentionally kept out of this header (and every other public
// header) to keep it out of the include graph of files that don't need it -
// only daemon_server.cpp pays for it. See DaemonClient for the matching
// client-side wrapper.
class DaemonServer {
public:
    // model_path/gpu_layers are forwarded to ai::LlamaClient as-is (see
    // config::MygitConfig - the caller is expected to have already resolved
    // these via config::load_config()).
    explicit DaemonServer(std::string model_path, int gpu_layers, int port = resolve_port());
    ~DaemonServer();

    DaemonServer(const DaemonServer&) = delete;
    DaemonServer& operator=(const DaemonServer&) = delete;

    // Binds 127.0.0.1:<port> and serves requests until POST /shutdown is
    // received, a SIGINT/SIGTERM (or Windows equivalent) is caught, or the
    // idle timeout (kIdleTimeoutSeconds with no /review or /commit activity)
    // fires. Blocks the calling thread for the daemon's whole lifetime.
    void run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mygit::daemon
