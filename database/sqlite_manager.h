#pragma once

#include <string>

namespace mygit::database {

// Backing store for the future Memory System (previous bugs, fixed issues,
// review history). Not yet wired into the review pipeline - V1 only opens
// the database file so the schema can be designed incrementally.
class SqliteManager {
public:
    explicit SqliteManager(std::string db_path = "logs/mygit.db");
    ~SqliteManager();

    SqliteManager(const SqliteManager&) = delete;
    SqliteManager& operator=(const SqliteManager&) = delete;

    bool is_open() const;

private:
    std::string db_path_;
    void* db_ = nullptr;  // sqlite3*, kept opaque to avoid leaking <sqlite3.h> into the header.
};

}  // namespace mygit::database
