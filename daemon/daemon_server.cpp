#include "daemon/daemon_server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <thread>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"

namespace mygit::daemon {

namespace {

// Set for the duration of run() so OS signal handlers (which can't capture
// state) can reach the listening server to unblock listen() and free the
// port. Only one DaemonServer runs per process, so a single static pointer
// is sufficient.
std::atomic<httplib::Server*> g_server_for_signal{nullptr};

void handle_termination_signal(int) {
    if (httplib::Server* server = g_server_for_signal.load()) {
        server->stop();
    }
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Builds the ai::SplitPrompt/plain-prompt pair from a JSON request body.
// Requests may send either {"prompt": "..."} (plain, no prefix caching) or
// {"prefix": "...", "suffix": "..."} (Level 1 prefix KV caching - see
// LlamaClient::cache_system_prefix). The daemon is what makes the split form
// actually pay off: the prefix's KV state now persists across HTTP requests
// and CLI invocations, not just within one process's lifetime.
struct ParsedPrompt {
    bool is_split = false;
    std::string prompt;
    ai::SplitPrompt split;
};

ParsedPrompt parse_prompt(const nlohmann::json& body) {
    ParsedPrompt parsed;
    if (body.contains("prefix") || body.contains("suffix")) {
        parsed.is_split = true;
        parsed.split.prefix = body.value("prefix", "");
        parsed.split.suffix = body.value("suffix", "");
    } else {
        parsed.prompt = body.at("prompt").get<std::string>();
    }
    return parsed;
}

}  // namespace

struct DaemonServer::Impl {
    Impl(std::string model_path, int gpu_layers, int port_in)
        : port(port_in), llama_client(std::move(model_path), gpu_layers) {
        last_activity_ms.store(now_ms());
    }

    int port;
    ai::LlamaClient llama_client;

    // LlamaClient owns a single llama_context/KV cache and is not
    // thread-safe; httplib dispatches requests on a worker pool, so every
    // inference call must be serialized through this mutex.
    std::mutex inference_mutex;

    httplib::Server server;

    std::atomic<int64_t> last_activity_ms{0};
    std::atomic<bool> idle_timer_running{false};
    std::thread idle_timer_thread;
    // Lets stop_idle_timer() wake the watcher immediately instead of it
    // sitting in a 30s sleep - otherwise /shutdown, SIGINT/SIGTERM, etc.
    // would report success right away but the process could still take up
    // to kPollInterval to actually exit and free the port.
    std::mutex idle_cv_mutex;
    std::condition_variable idle_cv;

    void touch_activity() { last_activity_ms.store(now_ms()); }

    void setup_routes();
    void start_idle_timer();
    void stop_idle_timer();
};

void DaemonServer::Impl::setup_routes() {
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    server.Post("/review", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            const ParsedPrompt parsed = parse_prompt(nlohmann::json::parse(req.body));
            std::string result;
            {
                std::lock_guard<std::mutex> lock(inference_mutex);
                result = parsed.is_split ? llama_client.review(parsed.split)
                                          : llama_client.review(parsed.prompt);
            }
            touch_activity();
            res.set_content(result, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    server.Post("/commit", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            const ParsedPrompt parsed = parse_prompt(nlohmann::json::parse(req.body));
            std::string result;
            {
                std::lock_guard<std::mutex> lock(inference_mutex);
                result = parsed.is_split ? llama_client.generate_commit_message(parsed.split)
                                          : llama_client.generate_commit_message(parsed.prompt);
            }
            touch_activity();
            res.set_content(result, "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    server.Post("/shutdown", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"stopping"})", "application/json");
        server.stop();
    });
}

// Wakes periodically and stops the server once kIdleTimeoutSeconds have
// passed with no /review or /commit activity, freeing the model's
// RAM/VRAM. Runs until idle_timer_running is cleared (by run(), after
// listen() returns for any reason).
void DaemonServer::Impl::start_idle_timer() {
    idle_timer_running.store(true);
    idle_timer_thread = std::thread([this] {
        constexpr auto kPollInterval = std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(idle_cv_mutex);
        while (idle_timer_running.load()) {
            // Waits up to kPollInterval, but returns immediately once
            // stop_idle_timer() clears idle_timer_running and notifies -
            // that's what keeps /shutdown (and SIGINT/SIGTERM, and the
            // idle timeout itself) from leaving the process to linger for
            // up to 30s after it's already told the caller it stopped.
            idle_cv.wait_for(lock, kPollInterval, [this] { return !idle_timer_running.load(); });
            if (!idle_timer_running.load()) return;

            const int64_t idle_ms = now_ms() - last_activity_ms.load();
            if (idle_ms >= static_cast<int64_t>(kIdleTimeoutSeconds) * 1000) {
                spdlog::info("daemon: idle for {}s, shutting down", idle_ms / 1000);
                server.stop();
                return;
            }
        }
    });
}

void DaemonServer::Impl::stop_idle_timer() {
    {
        std::lock_guard<std::mutex> lock(idle_cv_mutex);
        idle_timer_running.store(false);
    }
    idle_cv.notify_all();
    if (idle_timer_thread.joinable()) {
        idle_timer_thread.join();
    }
}

DaemonServer::DaemonServer(std::string model_path, int gpu_layers, int port)
    : impl_(std::make_unique<Impl>(std::move(model_path), gpu_layers, port)) {
    impl_->setup_routes();
}

DaemonServer::~DaemonServer() = default;

void DaemonServer::run() {
    g_server_for_signal.store(&impl_->server);
    std::signal(SIGINT, handle_termination_signal);
    std::signal(SIGTERM, handle_termination_signal);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, handle_termination_signal);  // Windows Ctrl+Break
#endif

    impl_->start_idle_timer();

    spdlog::info("mygit daemon listening on 127.0.0.1:{}", impl_->port);
    impl_->server.listen("127.0.0.1", impl_->port);

    // listen() returned - either /shutdown, a signal, or the idle timer
    // stopped the server. Tear the watcher thread down so the port and
    // process can exit cleanly (spec: "the port is freed").
    impl_->stop_idle_timer();
    g_server_for_signal.store(nullptr);
}

}  // namespace mygit::daemon
