#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "mygit/types.h"

namespace mygit::ui {

// Call once at startup on Windows to enable ANSI escape code processing.
// No-op on Linux/macOS.
void enable_colors();

// Shows a spinning animation on a background thread while a slow operation
// (model load + inference) runs on the main thread.
// Clears itself from the terminal when stop() or the destructor is called.
class Spinner {
public:
    explicit Spinner(std::string message);
    ~Spinner();

    // Stops the animation and clears the line. Safe to call multiple times.
    void stop();

private:
    std::string message_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

// Prints a formatted, colored review report to stdout.
void print_report(const ReviewResult& result);

// Prints a one-line verdict (✓ passed / ✗ blocked / ⚠ forced).
void print_verdict(bool allowed, bool force_ai_used);

}  // namespace mygit::ui
