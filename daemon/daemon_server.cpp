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
#include <memory>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

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
        : port(port_in), hosted(hosted_mode()), stub(hosted && env_is("MYGIT_INFERENCE_BACKEND", "stub")) {
        if (hosted) {
            const char* token = std::getenv("MYGIT_API_TOKEN");
            if (!token || std::string(token).size() < 24)
                throw std::runtime_error("Hosted mode requires MYGIT_API_TOKEN with at least 24 characters");
            api_token = token;
            if (!stub) throw std::runtime_error("Hosted mode currently supports only MYGIT_INFERENCE_BACKEND=stub; configure a real backend before advertising inference");
        } else {
            llama_client = std::make_unique<ai::LlamaClient>(std::move(model_path), gpu_layers);
        }
#if defined(_WIN32)
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        last_activity_ms.store(now_ms());
    }

    ~Impl() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    int port;
    bool hosted;
    bool stub;
    std::string api_token;
    std::unique_ptr<ai::LlamaClient> llama_client;

    // LlamaClient owns a single llama_context/KV cache and is not
    // thread-safe; httplib dispatches requests on a worker pool, so every
    // inference call must be serialized through this mutex.
    std::mutex inference_mutex;
    std::mutex rate_mutex;
    int64_t rate_window_ms = 0;
    int rate_requests = 0;

    httplib::Server server;

    static bool env_is(const char* key, const char* expected) {
        const char* value = std::getenv(key);
        return value && std::string(value) == expected;
    }

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
    bool rate_allowed() {
        if (!hosted) return true;
        std::lock_guard<std::mutex> lock(rate_mutex);
        const int64_t now = now_ms();
        if (now - rate_window_ms >= 60000) { rate_window_ms = now; rate_requests = 0; }
        return ++rate_requests <= 30;
    }

    void setup_routes();
    void start_idle_timer();
    void stop_idle_timer();
};

void DaemonServer::Impl::setup_routes() {
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(10, 0);
    server.set_write_timeout(30, 0);
    server.new_task_queue = [] { return new httplib::ThreadPool(2); };
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        const std::string id = req.get_header_value("X-Request-ID");
        spdlog::info("http request_id={} method={} path={} status={}",
                     id.empty() ? "none" : id, req.method, req.path, res.status);
    });
    auto authenticate = [this](const httplib::Request& req, httplib::Response& res) {
        if (hosted && req.get_header_value("Authorization") != "Bearer " + api_token) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return false;
        }
        return true;
    };
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    server.Get("/live", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"alive"})", "application/json");
    });
    server.Get("/ready", [this, authenticate](const httplib::Request& req, httplib::Response& res) {
        if (!authenticate(req, res)) return;
        res.status = 200;
        res.set_content(nlohmann::json{{"status", "ready"}, {"backend", stub ? "stub" : "llama"}}.dump(), "application/json");
    });

    server.Post("/review", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            if (hosted && req.get_header_value("Authorization") != "Bearer " + api_token) { res.status = 401; res.set_content(R"({"error":"unauthorized"})", "application/json"); return; }
            if (!rate_allowed()) { res.status = 429; res.set_content(R"({"error":"rate_limit_exceeded"})", "application/json"); return; }
            if (req.get_header_value("Content-Type").find("application/json") != 0) { res.status = 415; res.set_content(R"({"error":"content_type_must_be_application_json"})", "application/json"); return; }
            const ParsedPrompt parsed = parse_prompt(nlohmann::json::parse(req.body));
            std::string result;
            if (stub) result = R"({"safe":false,"issues":[{"severity":"critical","file":"","line":0,"message":"STUB BACKEND: no code review was performed; this result is synthetic."}],"backend":"stub"})";
            else {
                std::lock_guard<std::mutex> lock(inference_mutex);
                result = parsed.is_split ? llama_client->review(parsed.split)
                                          : llama_client->review(parsed.prompt);
            }
            touch_activity();
            res.set_content(result, "application/json");
        } catch (const nlohmann::json::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid_json_or_prompt"})", "application/json");
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(R"({"error":"inference_backend_unavailable"})", "application/json");
        }
    });

    server.Post("/commit", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            if (hosted && req.get_header_value("Authorization") != "Bearer " + api_token) { res.status = 401; res.set_content(R"({"error":"unauthorized"})", "application/json"); return; }
            if (!rate_allowed()) { res.status = 429; res.set_content(R"({"error":"rate_limit_exceeded"})", "application/json"); return; }
            if (req.get_header_value("Content-Type").find("application/json") != 0) { res.status = 415; res.set_content(R"({"error":"content_type_must_be_application_json"})", "application/json"); return; }
            const ParsedPrompt parsed = parse_prompt(nlohmann::json::parse(req.body));
            std::string result;
            if (stub) result = "test: stub backend (no real inference)";
            else {
                std::lock_guard<std::mutex> lock(inference_mutex);
                result = parsed.is_split ? llama_client->generate_commit_message(parsed.split)
                                          : llama_client->generate_commit_message(parsed.prompt);
            }
            touch_activity();
            res.set_content(result, "text/plain");
        } catch (const nlohmann::json::exception&) {
            res.status = 400;
            res.set_content(R"({"error":"invalid_json_or_prompt"})", "application/json");
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(R"({"error":"inference_backend_unavailable"})", "application/json");
        }
    });

    if (!hosted) server.Post("/shutdown", [this](const httplib::Request&, httplib::Response& res) {
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
    if (impl_->hosted && Impl::env_is("MYGIT_INFERENCE_BACKEND", "stub"))
        spdlog::warn("hosted API is using the STUB backend; responses are synthetic and are not code reviews");
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

    if (!impl_->hosted) impl_->start_idle_timer();

    const char* address = impl_->hosted ? "0.0.0.0" : "127.0.0.1";
    spdlog::info("mygit {} listening on {}:{}", impl_->hosted ? "hosted API" : "daemon", address, impl_->port);
    if (!impl_->server.listen(address, impl_->port)) {
        spdlog::error("Failed to bind to {}:{} - port may be in use or access denied", address, impl_->port);
    }

    // listen() returned - either /shutdown, a signal, or the idle timer
    // stopped the server. Tear the watcher thread down so the port and
    // process can exit cleanly (spec: "the port is freed").
    if (!impl_->hosted) impl_->stop_idle_timer();
    spdlog::info("daemon run() completed.");
    g_server_for_signal.store(nullptr);
}

}  // namespace mygit::daemon
