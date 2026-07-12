#include "database/sqlite_manager.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "git/git_runner.h"

namespace mygit::database {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SqliteManager::SqliteManager(std::string db_path) : db_path_(std::move(db_path)) {
    // Ensure parent directory exists.
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(db_path_).parent_path(), ec);
    }

    sqlite3* handle = nullptr;
    if (sqlite3_open(db_path_.c_str(), &handle) == SQLITE_OK) {
        db_ = handle;
        init_schema();
    } else if (handle != nullptr) {
        sqlite3_close(handle);
    }
}

SqliteManager::~SqliteManager() {
    if (db_ != nullptr) {
        sqlite3_close(static_cast<sqlite3*>(db_));
    }
}

bool SqliteManager::is_open() const {
    return db_ != nullptr;
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

void SqliteManager::init_schema() {
    if (!db_) return;

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS reviews (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     TEXT    NOT NULL DEFAULT (datetime('now')),
            branch        TEXT    NOT NULL,
            commit_hash   TEXT    NOT NULL DEFAULT '',
            files_changed INTEGER NOT NULL DEFAULT 0,
            issues_json   TEXT    NOT NULL DEFAULT '[]',
            blocked       INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS issues (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            review_id INTEGER NOT NULL REFERENCES reviews(id),
            severity  TEXT    NOT NULL,
            file      TEXT    NOT NULL,
            line      INTEGER NOT NULL DEFAULT 0,
            message   TEXT    NOT NULL
        );
    )";

    char* err_msg = nullptr;
    sqlite3_exec(static_cast<sqlite3*>(db_), sql, nullptr, nullptr, &err_msg);
    if (err_msg) {
        sqlite3_free(err_msg);
    }
}

// ---------------------------------------------------------------------------
// save_review
// ---------------------------------------------------------------------------

void SqliteManager::save_review(const ReviewResult& result,
                                 const std::string& branch,
                                 bool blocked) {
    if (!db_) return;
    auto* db = static_cast<sqlite3*>(db_);

    // -- Gather git metadata ------------------------------------------------
    const std::string commit_hash = git::run_git_capture("rev-parse HEAD");

    // Count unique files from the staged diff (more accurate than issue files).
    const std::string staged_files_raw = git::run_git_capture("diff --staged --name-only");
    int files_changed = 0;
    if (!staged_files_raw.empty()) {
        files_changed = 1;
        for (char c : staged_files_raw) {
            if (c == '\n') ++files_changed;
        }
    }

    // -- Serialize issues to JSON -------------------------------------------
    nlohmann::json issues_arr = nlohmann::json::array();
    for (const auto& issue : result.issues) {
        issues_arr.push_back({
            {"severity", to_string(issue.severity)},
            {"file",     issue.file},
            {"line",     issue.line},
            {"message",  issue.message}
        });
    }
    const std::string issues_json = issues_arr.dump();

    // -- Transaction --------------------------------------------------------
    char* err = nullptr;
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); return; }

    // -- INSERT into reviews ------------------------------------------------
    const char* insert_review_sql =
        "INSERT INTO reviews (branch, commit_hash, files_changed, issues_json, blocked) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, insert_review_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    sqlite3_bind_text(stmt, 1, branch.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, commit_hash.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, files_changed);
    sqlite3_bind_text(stmt, 4, issues_json.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 5, blocked ? 1 : 0);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }
    sqlite3_finalize(stmt);

    const int64_t review_id = sqlite3_last_insert_rowid(db);

    // -- INSERT each issue --------------------------------------------------
    const char* insert_issue_sql =
        "INSERT INTO issues (review_id, severity, file, line, message) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt* issue_stmt = nullptr;
    if (sqlite3_prepare_v2(db, insert_issue_sql, -1, &issue_stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    for (const auto& issue : result.issues) {
        sqlite3_reset(issue_stmt);
        sqlite3_clear_bindings(issue_stmt);

        const std::string sev = to_string(issue.severity);
        sqlite3_bind_int64(issue_stmt, 1, review_id);
        sqlite3_bind_text (issue_stmt, 2, sev.c_str(),           -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (issue_stmt, 3, issue.file.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (issue_stmt, 4, issue.line);
        sqlite3_bind_text (issue_stmt, 5, issue.message.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(issue_stmt) != SQLITE_DONE) {
            sqlite3_finalize(issue_stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return;
        }
    }
    sqlite3_finalize(issue_stmt);

    // -- Commit -------------------------------------------------------------
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// get_recent_reviews
// ---------------------------------------------------------------------------

std::vector<ReviewRecord> SqliteManager::get_recent_reviews(int limit) {
    std::vector<ReviewRecord> records;
    if (!db_) return records;

    auto* db = static_cast<sqlite3*>(db_);

    const char* sql =
        "SELECT r.id, r.timestamp, r.branch, r.commit_hash, r.files_changed, "
        "       r.blocked, "
        "       (SELECT COUNT(*) FROM issues i WHERE i.review_id = r.id) AS issues_count "
        "FROM reviews r "
        "ORDER BY r.timestamp DESC "
        "LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ReviewRecord rec;
        rec.id            = sqlite3_column_int64(stmt, 0);
        rec.timestamp     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.branch        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.commit_hash   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        rec.files_changed = sqlite3_column_int(stmt, 4);
        rec.blocked       = sqlite3_column_int(stmt, 5) != 0;
        rec.issues_count  = sqlite3_column_int(stmt, 6);
        records.push_back(std::move(rec));
    }

    sqlite3_finalize(stmt);
    return records;
}

}  // namespace mygit::database
