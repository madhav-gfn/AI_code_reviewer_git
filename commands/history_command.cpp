#include "commands/history_command.h"

#include <iostream>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/color.hpp>

#include "config/mygit_config.h"
#include "database/sqlite_manager.h"

namespace mygit::commands {

int run_history() {
    const auto db_path = config::get_config_dir() / "mygit.db";
    database::SqliteManager db(db_path.string());

    if (!db.is_open()) {
        std::cerr << "\n  Error: could not open review database.\n\n";
        return 1;
    }

    const auto reviews = db.get_recent_reviews(10);

    if (reviews.empty()) {
        using namespace ftxui;
        auto doc = hbox({
            text("  "),
            text(" ℹ ") | bold | color(Color::White) | bgcolor(Color::Blue),
            text("  No review history yet. Run ") | color(Color::GrayLight),
            text("mygit review") | bold | color(Color::Cyan),
            text(" to get started.") | color(Color::GrayLight),
        });
        auto screen = Screen::Create(Dimension::Fit(doc));
        Render(screen, doc);
        std::cout << "\n" << screen.ToString() << "\n\n";
        return 0;
    }

    using namespace ftxui;

    // Build table data: header + rows.
    std::vector<std::vector<std::string>> table_data;
    table_data.push_back({"#", "Timestamp", "Branch", "Commit", "Files", "Issues", "Blocked"});

    int row_num = 1;
    for (const auto& r : reviews) {
        // Truncate commit hash to 7 chars (standard short hash).
        std::string short_hash = r.commit_hash.size() > 7
            ? r.commit_hash.substr(0, 7)
            : r.commit_hash;
        if (short_hash.empty()) short_hash = "-";

        table_data.push_back({
            std::to_string(row_num++),
            r.timestamp,
            r.branch,
            short_hash,
            std::to_string(r.files_changed),
            std::to_string(r.issues_count),
            r.blocked ? "Y" : "N"
        });
    }

    auto table = Table(table_data);

    // -- Style the table ----------------------------------------------------
    table.SelectAll().Border(LIGHT);

    // Header row styling.
    table.SelectRow(0).Decorate(bold);
    table.SelectRow(0).DecorateCells(color(Color::Cyan));
    table.SelectRow(0).SeparatorVertical(LIGHT);

    // Alternate row shading for readability.
    for (int i = 1; i < static_cast<int>(table_data.size()); ++i) {
        if (i % 2 == 0) {
            table.SelectRow(i).DecorateCells(bgcolor(Color::RGB(30, 30, 40)));
        }
    }

    // "Blocked" column coloring: Y = red, N = green.
    const int blocked_col = 6;
    for (int i = 1; i < static_cast<int>(table_data.size()); ++i) {
        if (table_data[i][blocked_col] == "Y") {
            table.SelectCell(i, blocked_col).DecorateCells(color(Color::RedLight));
        } else {
            table.SelectCell(i, blocked_col).DecorateCells(color(Color::Green));
        }
    }

    // Title banner.
    auto title = hbox({
        text("  "),
        text(" 📋 ") | bold,
        text(" Review History ") | bold | color(Color::White),
        text("(last " + std::to_string(reviews.size()) + ")") | color(Color::GrayLight),
    });

    auto doc = vbox({
        title,
        text(""),
        table.Render() | flex,
    });

    auto screen = Screen::Create(Dimension::Fit(doc));
    Render(screen, doc);
    std::cout << "\n" << screen.ToString() << "\n\n";

    return 0;
}

}  // namespace mygit::commands
