#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mygit/types.h"

namespace mygit::database {

// A lightweight record returned by get_recent_reviews().
struct ReviewRecord {
    int64_t     id            = 0;
    std::string timestamp;        // ISO-8601 from SQLite (e.g. "2026-07-12 17:30:00")
    std::string branch;
    std::string commit_hash;
    int         files_changed = 0;
    int         issues_count  = 0; // derived: COUNT from issues table
    bool        blocked       = false;
};

// Deterministic, dependency-free fingerprint of a file's diff content
// (path + patch text). Used as the cache key for get_cached_file_review()/
// save_cached_file_review() — two diffs with the same fingerprint are
// byte-for-byte identical, so a review of one can stand in for the other.
std::string hash_diff_content(const std::string& path, const std::string& patch);

// Backing store for the V3 Memory System — review history, issues,
// and (future) learned patterns. Uses RAII; the database is opened
// in the constructor and closed in the destructor. Schema is created
// automatically on first run.
class SqliteManager {
public:
    explicit SqliteManager(std::string db_path = "logs/mygit.db");
    ~SqliteManager();

    SqliteManager(const SqliteManager&) = delete;
    SqliteManager& operator=(const SqliteManager&) = delete;

    bool is_open() const;

    // Persists a review result and its individual issues to the database.
    // Uses prepared statements inside a transaction for atomicity and
    // SQL-injection safety.
    void save_review(const ReviewResult& result, const std::string& branch,
                     bool blocked);

    // Returns the last `limit` reviews (most-recent first).
    std::vector<ReviewRecord> get_recent_reviews(int limit = 10);

    // Looks up a previously-computed review for a diff fingerprint (see
    // hash_diff_content). Returns true and fills `out` on a hit, letting the
    // caller skip re-running inference on a diff it has already reviewed.
    bool get_cached_file_review(const std::string& hash, ReviewResult& out);

    // Stores `result` under `hash` for future reuse by get_cached_file_review().
    void save_cached_file_review(const std::string& hash, const ReviewResult& result);

private:
    std::string db_path_;
    void* db_ = nullptr;  // sqlite3*, kept opaque to avoid leaking <sqlite3.h> into the header.

    // Creates the reviews and issues tables if they don't already exist.
    void init_schema();
};

}  // namespace mygit::database
