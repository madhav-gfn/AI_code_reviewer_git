#include "database/sqlite_manager.h"

#include <sqlite3.h>

namespace mygit::database {

SqliteManager::SqliteManager(std::string db_path) : db_path_(std::move(db_path)) {
    sqlite3* handle = nullptr;
    if (sqlite3_open(db_path_.c_str(), &handle) == SQLITE_OK) {
        db_ = handle;
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

}  // namespace mygit::database
