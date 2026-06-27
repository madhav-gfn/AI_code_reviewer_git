#include "ui/terminal_ui.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "mygit/types.h"

namespace mygit::ui {

namespace {

// ANSI color/style codes.
constexpr const char* kReset  = "\033[0m";
constexpr const char* kBold   = "\033[1m";
constexpr const char* kDim    = "\033[2m";
constexpr const char* kRed    = "\033[31m";
constexpr const char* kGreen  = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kCyan   = "\033[36m";

const char* severity_color(Severity s) {
    switch (s) {
        case Severity::Critical: return kRed;
        case Severity::High:     return kRed;
        case Severity::Medium:   return kYellow;
        case Severity::Low:      return kDim;
    }
    return kReset;
}

// Fixed-width labels keep the file:line column aligned.
const char* severity_label(Severity s) {
    switch (s) {
        case Severity::Critical: return "CRITICAL";
        case Severity::High:     return "HIGH    ";
        case Severity::Medium:   return "MEDIUM  ";
        case Severity::Low:      return "LOW     ";
    }
    return "UNKNOWN ";
}

}  // namespace

void enable_colors() {
#if defined(_WIN32)
    // Required for ANSI escape codes to render on Windows consoles.
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

// ---------------------------------------------------------------------------
// Spinner
// ---------------------------------------------------------------------------

Spinner::Spinner(std::string message) : message_(std::move(message)) {
    thread_ = std::thread([this]() {
        constexpr const char* kFrames[] = {"|", "/", "-", "\\"};
        int frame = 0;
        while (running_.load()) {
            std::cout << "\r  " << kCyan << kFrames[frame % 4] << kReset
                      << "  " << message_ << std::flush;
            ++frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    });
}

Spinner::~Spinner() { stop(); }

void Spinner::stop() {
    if (running_.exchange(false)) {
        thread_.join();
        // Overwrite the spinner line with spaces, then return to column 0.
        const std::string blank(message_.size() + 10, ' ');
        std::cout << "\r" << blank << "\r" << std::flush;
    }
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

void print_report(const ReviewResult& result) {
    if (result.issues.empty()) {
        std::cout << "\n  " << kGreen << kBold << "v" << kReset
                  << "  No issues found.\n\n";
        return;
    }

    std::cout << "\n";
    for (const auto& issue : result.issues) {
        const char* color = severity_color(issue.severity);
        std::cout
            << "  " << color << kBold << "*" << kReset << "  "
            << color << kBold << severity_label(issue.severity) << kReset
            << "  " << kCyan << issue.file << ":" << issue.line << kReset << "\n"
            << "     " << issue.message << "\n\n";
    }
}

void print_verdict(bool allowed, bool force_ai_used) {
    if (!allowed) {
        std::cout << "  " << kRed << kBold << "x" << kReset
                  << kRed << "  Blocked" << kReset
                  << " -- critical issues found.\n"
                  << "     Use " << kBold << "--force-ai" << kReset
                  << " to override.\n\n";
    } else if (force_ai_used) {
        std::cout << "  " << kYellow << "!" << kReset
                  << kYellow << "  Force override active -- proceeding.\n"
                  << kReset << "\n";
    } else {
        std::cout << "  " << kGreen << kBold << "v" << kReset
                  << kGreen << "  Review passed." << kReset << "\n\n";
    }
}

}  // namespace mygit::ui
