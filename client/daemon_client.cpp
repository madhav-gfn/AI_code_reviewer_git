#include "daemon_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <filesystem>

#include "config/mygit_config.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mygit::daemon {

namespace {

constexpr int kHealthCheckReadTimeoutSec = 2;
constexpr int kWorkReadTimeoutSec = 300;  // inference can be slow
constexpr int kAutoStartTimeoutSec = 120;

bool hosted_client() {
    const char* mode = std::getenv("MYGIT_BACKEND");
    if (!mode || !*mode || std::string(mode) == "local") return false;
    if (std::string(mode) == "hosted") return true;
    throw std::runtime_error("MYGIT_BACKEND must be either local or hosted");
}

std::string hosted_url() {
    const char* url = std::getenv("MYGIT_API_URL");
    if (!url || std::string(url).rfind("https://", 0) != 0)
        throw std::runtime_error("Hosted mode requires MYGIT_API_URL beginning with https://");
    return url;
}

httplib::Headers auth_headers() {
    httplib::Headers headers;
    static std::atomic<unsigned long long> request_id{0};
    headers.emplace("X-Request-ID", "mygit-" + std::to_string(++request_id));
    if (hosted_client()) {
        const char* token = std::getenv("MYGIT_API_TOKEN");
        if (!token || !*token) throw std::runtime_error("Hosted mode requires MYGIT_API_TOKEN");
        headers.emplace("Authorization", std::string("Bearer ") + token);
    }
    return headers;
}

httplib::Client make_client(int port, int read_timeout_sec) {
    httplib::Client cli = hosted_client() ? httplib::Client(hosted_url()) : httplib::Client("127.0.0.1", port);
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

#if defined(_WIN32)
std::string quote_windows_arg(const std::string& arg) {
    std::string quoted = "\"";
    size_t slashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++slashes;
        } else if (ch == '"') {
            quoted.append(slashes * 2 + 1, '\\');
            quoted += ch;
            slashes = 0;
        } else {
            quoted.append(slashes, '\\');
            quoted += ch;
            slashes = 0;
        }
    }
    quoted.append(slashes * 2, '\\');
    quoted += '"';
    return quoted;
}
#else
std::string quote_shell_arg(const std::string& arg) {
    std::string quoted = "'";
    for (char ch : arg) {
        if (ch == '\'') quoted += "'\\''";
        else quoted += ch;
    }
    quoted += '\'';
    return quoted;
}
#endif

std::string make_request_error(const char* endpoint, const httplib::Result& res) {
    return std::string("Daemon ") + endpoint + " request failed: " +
           (res ? res->body : std::string("no response (daemon unreachable)"));
}

}  // namespace

DaemonClient::DaemonClient(int port) : port_(port) {}

bool hosted_backend_selected() { return hosted_client(); }

bool DaemonClient::is_running() const {
    httplib::Client cli = make_client(port_, kHealthCheckReadTimeoutSec);
    for (int attempt = 0; attempt < (hosted_client() ? 3 : 1); ++attempt) {
        auto res = hosted_client() ? cli.Get("/live", auth_headers()) : cli.Get("/health");
        if (res && res->status == 200) return true;
        if (hosted_client() && attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

bool wait_for_daemon_ready(int port, int timeout_seconds) {
    if (hosted_client()) {
        auto cli = make_client(port, kHealthCheckReadTimeoutSec);
        const auto hosted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        do {
            auto res = cli.Get("/ready", auth_headers());
            if (res && res->status == 200) return true;
            if (res && (res->status == 401 || res->status == 403)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } while (std::chrono::steady_clock::now() < hosted_deadline);
        return false;
    }
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
    std::filesystem::path daemon_path = current_executable_path();
#if defined(_WIN32)
    daemon_path.replace_filename("mygit_daemon.exe");
#else
    daemon_path.replace_filename("mygit_daemon");
#endif
    const std::string exe = daemon_path.string();
    const config::MygitConfig config = config::load_config();
#if defined(_WIN32)
    std::string cmdline = quote_windows_arg(exe) + " --model-path " +
                          quote_windows_arg(config.model_path) + " --gpu-layers " +
                          std::to_string(config.gpu_layers);

    // The detached process has no console, so stdout/stderr are invalid
    // handles. Model loading (llama.cpp), spdlog, and ONNX Runtime all
    // write to stdout/stderr — those writes crash the process when the
    // handles are invalid. Redirect both to a log file so the daemon
    // survives and we get diagnostics on failure.
    const char* profile = std::getenv("USERPROFILE");
    std::string log_path;
    if (profile) {
        log_path = std::string(profile) + "\\.mygit\\daemon.log";
    } else {
        log_path = "daemon.log";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileA(log_path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLog;
        si.hStdError  = hLog;
        si.hStdInput  = nullptr;
    }

    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS: no console attached, so the daemon survives the
    // parent CLI process exiting and doesn't print to (or get Ctrl+C'd via)
    // the user's terminal. CREATE_NEW_PROCESS_GROUP so console signals sent
    // to this process don't propagate to the daemon either.
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                        hLog != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
#else
    // Simple POSIX fallback: background the process via the shell so it
    // detaches from the current terminal session.
    const std::string cmd = quote_shell_arg(exe) + " --model-path " +
                            quote_shell_arg(config.model_path) + " --gpu-layers " +
                            std::to_string(config.gpu_layers) + " >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

int run_daemon_foreground() {
    std::filesystem::path daemon_path = current_executable_path();
#if defined(_WIN32)
    daemon_path.replace_filename("mygit_daemon.exe");
#else
    daemon_path.replace_filename("mygit_daemon");
#endif
    const std::string exe = daemon_path.string();
    const config::MygitConfig config = config::load_config();
#if defined(_WIN32)
    std::string cmdline = quote_windows_arg(exe) + " --model-path " +
                          quote_windows_arg(config.model_path) + " --gpu-layers " +
                          std::to_string(config.gpu_layers);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("Could not start the local daemon executable: " + exe);
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
#else
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("Could not fork the local daemon process");
    if (child == 0) {
        execl(exe.c_str(), exe.c_str(), "--model-path", config.model_path.c_str(),
              "--gpu-layers", std::to_string(config.gpu_layers).c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Waiting for the local daemon process failed");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

bool DaemonClient::ensure_running() const {
    if (is_running()) return true;
    if (hosted_client()) return wait_for_daemon_ready(port_, kAutoStartTimeoutSec);
    spawn_detached_daemon();
    return wait_for_daemon_ready(port_, kAutoStartTimeoutSec);
}

std::string DaemonClient::review(const std::string& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prompt", prompt}};
    auto res = cli.Post("/review", auth_headers(), body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/review", res));
    return res->body;
}

std::string DaemonClient::review(const ai::SplitPrompt& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prefix", prompt.prefix}, {"suffix", prompt.suffix}};
    auto res = cli.Post("/review", auth_headers(), body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/review", res));
    return res->body;
}

std::string DaemonClient::generate_commit_message(const std::string& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prompt", prompt}};
    auto res = cli.Post("/commit", auth_headers(), body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/commit", res));
    return res->body;
}

std::string DaemonClient::generate_commit_message(const ai::SplitPrompt& prompt) const {
    httplib::Client cli = make_client(port_, kWorkReadTimeoutSec);
    const nlohmann::json body = {{"prefix", prompt.prefix}, {"suffix", prompt.suffix}};
    auto res = cli.Post("/commit", auth_headers(), body.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error(make_request_error("/commit", res));
    return res->body;
}

bool DaemonClient::shutdown() const {
    if (hosted_client()) return false;
    httplib::Client cli = make_client(port_, kHealthCheckReadTimeoutSec);
    auto res = cli.Post("/shutdown");
    return res && res->status == 200;
}

}  // namespace mygit::daemon
