#include "database/sqlite_manager.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "git/git_runner.h"

namespace mygit::database {

namespace {

nlohmann::json issues_to_json(const std::vector<Issue>& issues) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& issue : issues) {
        arr.push_back({
            {"severity", to_string(issue.severity)},
            {"file",     issue.file},
            {"line",     issue.line},
            {"message",  issue.message}
        });
    }
    return arr;
}

std::vector<Issue> issues_from_json(const std::string& issues_json) {
    std::vector<Issue> issues;
    const nlohmann::json arr = nlohmann::json::parse(issues_json, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) {
        return issues;
    }
    for (const auto& item : arr) {
        Issue issue;
        issue.severity = severity_from_string(item.value("severity", "low"));
        issue.file     = item.value("file", "");
        issue.line     = item.value("line", 0);
        issue.message  = item.value("message", "");
        issues.push_back(std::move(issue));
    }
    return issues;
}

}  // namespace

// ---------------------------------------------------------------------------
// hash_diff_content
// ---------------------------------------------------------------------------

std::string hash_diff_content(const std::string& path, const std::string& patch) {
    // FNV-1a 64-bit over path + a NUL separator + patch text. This is a
    // content-addressed cache key, not a security digest, so a fast
    // dependency-free hash is preferable to pulling in a crypto library.
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;

    uint64_t h = kOffsetBasis;
    auto mix = [&h](unsigned char c) {
        h ^= c;
        h *= kPrime;
    };
    for (unsigned char c : path) mix(c);
    mix('\0');
    for (unsigned char c : patch) mix(c);

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

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
        CREATE TABLE IF NOT EXISTS file_review_cache (
            hash        TEXT PRIMARY KEY,
            safe        INTEGER NOT NULL,
            issues_json TEXT    NOT NULL,
            timestamp   TEXT    NOT NULL DEFAULT (datetime('now'))
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
    const std::string issues_json = issues_to_json(result.issues).dump();

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

// ---------------------------------------------------------------------------
// Diff-content review cache
// ---------------------------------------------------------------------------

bool SqliteManager::get_cached_file_review(const std::string& hash, ReviewResult& out) {
    if (!db_) return false;
    auto* db = static_cast<sqlite3*>(db_);

    const char* sql = "SELECT safe, issues_json FROM file_review_cache WHERE hash = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.safe = sqlite3_column_int(stmt, 0) != 0;
        const unsigned char* text = sqlite3_column_text(stmt, 1);
        out.issues = issues_from_json(text ? reinterpret_cast<const char*>(text) : "[]");
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

void SqliteManager::save_cached_file_review(const std::string& hash, const ReviewResult& result) {
    if (!db_) return;
    auto* db = static_cast<sqlite3*>(db_);

    const std::string issues_json = issues_to_json(result.issues).dump();

    const char* sql =
        "INSERT INTO file_review_cache (hash, safe, issues_json, timestamp) "
        "VALUES (?, ?, ?, datetime('now')) "
        "ON CONFLICT(hash) DO UPDATE SET safe = excluded.safe, "
        "issues_json = excluded.issues_json, timestamp = excluded.timestamp;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, result.safe ? 1 : 0);
    sqlite3_bind_text(stmt, 3, issues_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

}  // namespace mygit::database
