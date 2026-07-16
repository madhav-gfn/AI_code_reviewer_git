#include "daemon/daemon_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mygit::daemon {

namespace {

constexpr int kHealthCheckReadTimeoutSec = 2;
constexpr int kWorkReadTimeoutSec = 300;  // inference can be slow
constexpr int kAutoStartTimeoutSec = 30;

httplib::Client make_client(int port, int read_timeout_sec) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);  // localhost - 1s to connect is generous
    cli.set_read_timeout(read_timeout_sec, 0);
    cli.set_write_timeout(30, 0);
    return cli;
}

// Resolves the path to the currently running mygit executable, so the
// auto-start path relaunches the same binary rather than relying on PATH.
#if defined(_WIN32)
std::string current_executable_path() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
}
#else
std::string current_executable_path() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "mygit";  // fall back to PATH lookup
    return std::string(buf, static_cast<size_t>(n));
}
#endif

std::string make_request_error(const char* endpoint, const httplib::Result& res) {
    return std::string("Daemon ") + endpoint + " request failed: " +
           (res ? res->body : std::string("no response (daemon unreachable)"));
}

}  // namespace

DaemonClient::DaemonClient(int port) : port_(port) {}

bool DaemonClient::is_running() const {
    httplib::Client cli = make_client(port_, kHealthCheckReadTimeoutSec);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

bool wait_for_daemon_ready(int port, int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    do {
        httplib::Client cli = make_client(port, kHealthCheckReadTimeoutSec);
        auto res = cli.Get("/health");
        if (res && res->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void spawn_detached_daemon() {
    const std::string exe = current_executable_path();
#if defined(_WIN32)
    std::string cmdline = "\"" + exe + "\" daemon start";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS: no console attached, so the daemon survives the
    // parent CLI process exiting and doesn't print to (or get Ctrl+C'd via)
    // the user's terminal. CREATE_NEW_PROCESS_GROUP so console signals sent
    // to this process don't propagate to the daemon either.
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    // Simple POSIX fallback: background the process via the shell so it
    // detaches from the current terminal session.
    const std::string cmd = "\"" + exe + "\" daemon start >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

bool DaemonClient::ensure_running() const {
    if (is_running()) return true;
    spawn_detached_daemon();
    return wait_for_daemon_ready(port_, kAutoStartTimeoutSec);
}

std::string DaemonClient::review(const std::string& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prompt", prompt}};
    auto res = cli.Post("/review", body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/review", res));
    return res->body;
}

std::string DaemonClient::review(const ai::SplitPrompt& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prefix", prompt.prefix}, {"suffix", prompt.suffix}};
    auto res = cli.Post("/review", body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/review", res));
    return res->body;
}

std::string DaemonClient::generate_commit_message(const std::string& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prompt", prompt}};
    auto res = cli.Post("/commit", body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/commit", res));
    return res->body;
}

std::string DaemonClient::generate_commit_message(const ai::SplitPrompt& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prefix", prompt.prefix}, {"suffix", prompt.suffix}};
    auto res = cli.Post("/commit", body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/commit", res));
    return res->body;
}

bool DaemonClient::shutdown() const {
    httplib::Client cli = make_client(port_, kHealthCheckReadTimeoutSec);
    auto res = cli.Post("/shutdown");
    return res && res->status == 200;
}

}  // namespace mygit::daemon
