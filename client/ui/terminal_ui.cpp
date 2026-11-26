#include "ui/terminal_ui.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/color.hpp>

#include "mygit/types.h"

namespace mygit::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

using namespace ftxui;

/// Map a severity to an FTXUI foreground color.
Color severity_fg(Severity s) {
    switch (s) {
        case Severity::Critical: return Color::RedLight;
        case Severity::High:     return Color::Red;
        case Severity::Medium:   return Color::Yellow;
        case Severity::Low:      return Color::GrayDark;
    }
    return Color::White;
}

/// Map a severity to a background color for the badge.
Color severity_bg(Severity s) {
    switch (s) {
        case Severity::Critical: return Color::Red;
        case Severity::High:     return Color::RGB(180, 60, 60);
        case Severity::Medium:   return Color::RGB(180, 140, 20);
        case Severity::Low:      return Color::RGB(80, 80, 80);
    }
    return Color::GrayDark;
}

/// Short label for the badge.
std::string severity_label(Severity s) {
    switch (s) {
        case Severity::Critical: return " CRITICAL ";
        case Severity::High:     return " HIGH ";
        case Severity::Medium:   return " MEDIUM ";
        case Severity::Low:      return " LOW ";
    }
    return " UNKNOWN ";
}

/// Build a single issue row as an Element (severity badge + file:line + message).
Element render_issue(const Issue& issue) {
    auto badge = text(severity_label(issue.severity))
                 | bold
                 | color(Color::White)
                 | bgcolor(severity_bg(issue.severity));

    auto location = text(issue.file + ":" + std::to_string(issue.line))
                    | color(Color::Cyan);

    auto msg = paragraph(issue.message)
               | color(Color::GrayLight);

    return vbox({
        hbox({badge, text("  "), location}),
        hbox({text("    "), msg}),
    });
}

/// Build the summary bar counting issues by severity.
Element render_summary(const ReviewResult& result) {
    int n_critical = 0, n_high = 0, n_medium = 0, n_low = 0;
    for (const auto& issue : result.issues) {
        switch (issue.severity) {
            case Severity::Critical: ++n_critical; break;
            case Severity::High:     ++n_high;     break;
            case Severity::Medium:   ++n_medium;   break;
            case Severity::Low:      ++n_low;      break;
        }
    }

    Elements counts;
    if (n_critical > 0) {
        counts.push_back(
            text(" " + std::to_string(n_critical) + " critical ")
            | bold | color(Color::White) | bgcolor(Color::Red));
        counts.push_back(text("  "));
    }
    if (n_high > 0) {
        counts.push_back(
            text(" " + std::to_string(n_high) + " high ")
            | bold | color(Color::White) | bgcolor(Color::RGB(180, 60, 60)));
        counts.push_back(text("  "));
    }
    if (n_medium > 0) {
        counts.push_back(
            text(" " + std::to_string(n_medium) + " medium ")
            | bold | color(Color::White) | bgcolor(Color::RGB(180, 140, 20)));
        counts.push_back(text("  "));
    }
    if (n_low > 0) {
        counts.push_back(
            text(" " + std::to_string(n_low) + " low ")
            | color(Color::White) | bgcolor(Color::RGB(80, 80, 80)));
    }

    auto total_str = std::to_string(result.issues.size()) + " issue"
                     + (result.issues.size() != 1 ? "s" : "");

    return hbox({
        text(" " + total_str + "  ") | bold | color(Color::White),
        separator() | color(Color::GrayDark),
        text("  "),
        hbox(std::move(counts)),
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// enable_colors
// ---------------------------------------------------------------------------

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
// Spinner  (FTXUI animated spinner, rendered in a background thread)
// ---------------------------------------------------------------------------

Spinner::Spinner(std::string message) : message_(std::move(message)) {
    thread_ = std::thread([this]() {
        using namespace ftxui;
        size_t frame = 0;
        while (running_.load()) {
            // FTXUI provides multiple spinner charsets; charset 18 is a
            // smooth braille-dot spinner.
            auto spin_elem = spinner(18, frame);
            auto doc = hbox({
                text("  "),
                spin_elem | bold | color(Color::Cyan),
                text("  " + message_),
            });

            auto screen = Screen::Create(Dimension::Fit(doc));
            Render(screen, doc);

            // Overwrite the current line.
            std::cout << "\r" << screen.ToString() << std::flush;

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
        const std::string blank(message_.size() + 20, ' ');
        std::cout << "\r" << blank << "\r" << std::flush;
    }
}

// ---------------------------------------------------------------------------
// Report  (FTXUI bordered panel with issue cards + summary bar)
// ---------------------------------------------------------------------------

void print_report(const ReviewResult& result) {
    using namespace ftxui;

    if (result.issues.empty()) {
        auto doc = hbox({
            text("  "),
            text(" ✓ ") | bold | color(Color::White) | bgcolor(Color::Green),
            text("  No issues found.") | bold | color(Color::Green),
        });
        auto screen = Screen::Create(Dimension::Fit(doc));
        Render(screen, doc);
        std::cout << "\n" << screen.ToString() << "\n\n";
        return;
    }

    // Build issue cards separated by light dividers.
    Elements issue_rows;
    for (size_t i = 0; i < result.issues.size(); ++i) {
        issue_rows.push_back(render_issue(result.issues[i]));
        if (i + 1 < result.issues.size()) {
            issue_rows.push_back(separatorLight() | color(Color::GrayDark));
        }
    }

    // Title for the window.
    auto title = text(" Code Review Report ") | bold;

    // Assemble: bordered window with issues + summary footer.
    auto report_body = vbox(std::move(issue_rows));

    auto summary = render_summary(result);

    auto panel = vbox({
        window(title, report_body),
        hbox({text("  "), summary}),
    });

    auto screen = Screen::Create(Dimension::Fit(panel));
    Render(screen, panel);
    std::cout << "\n" << screen.ToString() << "\n";
}

// ---------------------------------------------------------------------------
// Verdict
// ---------------------------------------------------------------------------

void print_verdict(bool allowed, bool force_ai_used) {
    using namespace ftxui;

    Element doc;
    if (!allowed) {
        doc = vbox({
            hbox({
                text("  "),
                text(" ✗ ") | bold | color(Color::White) | bgcolor(Color::Red),
                text("  Blocked") | bold | color(Color::Red),
                text(" — critical issues found.") | color(Color::Red),
            }),
            hbox({
                text("     Use "),
                text("--force-ai") | bold | color(Color::White),
                text(" to override."),
            }),
        });
    } else if (force_ai_used) {
        doc = hbox({
            text("  "),
            text(" ⚠ ") | bold | color(Color::White) | bgcolor(Color::Yellow),
            text("  Force override active — proceeding.") | color(Color::Yellow),
        });
    } else {
        doc = hbox({
            text("  "),
            text(" ✓ ") | bold | color(Color::White) | bgcolor(Color::Green),
            text("  Review passed.") | bold | color(Color::Green),
        });
    }

    auto screen = Screen::Create(Dimension::Fit(doc));
    Render(screen, doc);
    std::cout << screen.ToString() << "\n\n";
}

}  // namespace mygit::ui
