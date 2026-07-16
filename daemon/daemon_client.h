#pragma once

#include <string>

#include "ai/prompt_builder.h"
#include "daemon/daemon_config.h"

namespace mygit::daemon {

// Thin HTTP client used by the CLI commands to reach a background
// DaemonServer, so the model stays loaded across `mygit review`/`mygit
// commit` invocations instead of being loaded fresh by every process
// (Verdict #2 - zero cold starts). Method surface mirrors ai::LlamaClient's
// review()/generate_commit_message() overloads so callers can swap one for
// the other with minimal changes.
//
// httplib.h is kept out of this header; only daemon_client.cpp depends on
// it, per the project's header-hygiene rule for the daemon subsystem.
class DaemonClient {
public:
    explicit DaemonClient(int port = resolve_port());

    // GET /health. Returns true if a daemon answered within a short timeout.
    bool is_running() const;

    // Returns true if a daemon is already reachable, or if it wasn't and
    // spawn_detached_daemon() + polling brought one up within a few seconds.
    // Callers should treat `false` as "daemon unreachable and failed to
    // auto-start" and fail with a clear message rather than raw HTTP errors.
    bool ensure_running() const;

    // POSTs {"prompt": prompt} to /review and returns the raw JSON text
    // ai::LlamaClient::review() produced on the daemon side.
    std::string review(const std::string& prompt) const;

    // Split-prompt form (Level 1 prefix KV caching): posts
    // {"prefix": ..., "suffix": ...} to /review. The daemon evaluates the
    // prefix once and reuses its KV state across calls - including calls
    // from later CLI invocations, since the daemon outlives any single
    // process - so batched per-file reviews stay fast.
    std::string review(const ai::SplitPrompt& prompt) const;

    // POSTs {"prompt": prompt} to /commit and returns the generated commit
    // message text.
    std::string generate_commit_message(const std::string& prompt) const;
    std::string generate_commit_message(const ai::SplitPrompt& prompt) const;

    // POSTs /shutdown. Returns true if the daemon acknowledged the request.
    bool shutdown() const;

private:
    int port_;
};

// Spawns `<this executable> daemon start` as a fully detached background
// process (no console window; keeps running after the spawning process
// exits). Shared by DaemonClient::ensure_running()'s auto-start path and by
// `mygit daemon start -d`.
void spawn_detached_daemon();

// Polls GET /health on `port` every 500ms until it responds or
// `timeout_seconds` elapses. Returns true once the daemon is reachable.
bool wait_for_daemon_ready(int port, int timeout_seconds);

}  // namespace mygit::daemon
