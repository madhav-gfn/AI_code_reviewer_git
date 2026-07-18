#include "rag/vector_store.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

#include <sqlite3.h>

#include <faiss/IndexHNSW.h>
#include <faiss/IndexIDMap.h>
#include <faiss/index_io.h>

namespace mygit::rag {

namespace {

// HNSW graph fan-out. 32 is FAISS's own default and a reasonable balance of
// recall vs. memory/build time for a repo-sized (thousands, not millions of
// vectors) index.
constexpr int kHnswM = 32;

// Tombstone-driven rebuild thresholds (see VectorStore::remove_file):
// don't bother rebuilding tiny indexes, and only rebuild once a meaningful
// fraction of the index is dead weight.
constexpr int64_t kRebuildMinRows = 50;
constexpr double kRebuildTombstoneRatio = 0.3;

}  // namespace

struct VectorStore::Impl {
    std::string index_path;
    std::string db_path;

    sqlite3* db = nullptr;
    std::unique_ptr<faiss::IndexIDMap2> index;  // lazily created on first add_units()
    int64_t dim = 0;
    int64_t next_faiss_id = 0;

    Impl(std::string idx_path, std::string database_path)
        : index_path(std::move(idx_path)), db_path(std::move(database_path)) {
        {
            std::error_code ec;
            std::filesystem::create_directories(
                std::filesystem::path(db_path).parent_path(), ec);
            std::filesystem::create_directories(
                std::filesystem::path(index_path).parent_path(), ec);
        }

        if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "[rag] failed to open metadata database (" << db_path
                      << ") - repository context retrieval disabled.\n";
            if (db) { sqlite3_close(db); db = nullptr; }
            return;
        }
        init_schema();

        // Resume the id counter from whatever's already indexed.
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(faiss_id), -1) FROM rag_units;", -1,
                                &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                next_faiss_id = sqlite3_column_int64(stmt, 0) + 1;
            }
            sqlite3_finalize(stmt);
        }

        load_index_from_disk();
    }

    ~Impl() {
        if (db) sqlite3_close(db);
    }

    void init_schema() {
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS rag_units (
                faiss_id  INTEGER PRIMARY KEY,
                file_path TEXT NOT NULL,
                unit_name TEXT NOT NULL,
                content   TEXT NOT NULL,
                removed   INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS idx_rag_units_file_path ON rag_units(file_path);
            CREATE TABLE IF NOT EXISTS rag_index_state (
                file_path    TEXT PRIMARY KEY,
                content_hash TEXT NOT NULL,
                indexed_at   TEXT NOT NULL DEFAULT (datetime('now'))
            );
        )";
        char* err = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }

    void load_index_from_disk() {
        if (!std::filesystem::exists(index_path)) return;
        try {
            faiss::Index* loaded = faiss::read_index(index_path.c_str());
            auto* id_map = dynamic_cast<faiss::IndexIDMap2*>(loaded);
            if (!id_map) {
                delete loaded;
                std::cerr << "[rag] index file has an unexpected format (" << index_path
                          << ") - starting a fresh index.\n";
                return;
            }
            id_map->own_fields = true;
            index.reset(id_map);
            dim = index->d;
        } catch (const std::exception& e) {
            std::cerr << "[rag] failed to load vector index (" << e.what()
                       << ") - starting a fresh index.\n";
        }
    }

    void ensure_index(int64_t embedding_dim) {
        if (index) return;
        dim = embedding_dim;
        auto* hnsw = new faiss::IndexHNSWFlat(static_cast<int>(dim), kHnswM,
                                               faiss::METRIC_INNER_PRODUCT);
        index = std::make_unique<faiss::IndexIDMap2>(hnsw);
        index->own_fields = true;
    }

    // Reconstructs every surviving (non-tombstoned) vector from the current
    // index and re-adds them, under their original ids, to a brand-new
    // graph - the only way to actually shrink an HNSW index, since it has
    // no native removal.
    void rebuild() {
        if (!index || !db) return;

        std::vector<int64_t> surviving_ids;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT faiss_id FROM rag_units WHERE removed = 0 ORDER BY faiss_id;",
                                -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                surviving_ids.push_back(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }

        auto* fresh_hnsw =
            new faiss::IndexHNSWFlat(static_cast<int>(dim), kHnswM, faiss::METRIC_INNER_PRODUCT);
        auto fresh_index = std::make_unique<faiss::IndexIDMap2>(fresh_hnsw);
        fresh_index->own_fields = true;

        std::vector<float> vec(static_cast<size_t>(dim));
        for (int64_t id : surviving_ids) {
            try {
                index->reconstruct(id, vec.data());
                fresh_index->add_with_ids(1, vec.data(), &id);
            } catch (const std::exception&) {
                // Reconstruction failing for a stray id shouldn't abort the
                // whole rebuild - just drop that vector.
            }
        }

        index = std::move(fresh_index);

        char* err = nullptr;
        sqlite3_exec(db, "DELETE FROM rag_units WHERE removed = 1;", nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }

    void maybe_rebuild() {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                db, "SELECT COUNT(*), COALESCE(SUM(removed), 0) FROM rag_units;", -1, &stmt,
                nullptr) != SQLITE_OK) {
            return;
        }
        int64_t total = 0, removed = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
            removed = sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);

        if (total >= kRebuildMinRows &&
            static_cast<double>(removed) / static_cast<double>(total) > kRebuildTombstoneRatio) {
            rebuild();
            save();
        }
    }

    void save() const {
        if (!index) return;
        try {
            faiss::write_index(index.get(), index_path.c_str());
        } catch (const std::exception& e) {
            std::cerr << "[rag] failed to persist vector index: " << e.what() << "\n";
        }
    }
};

VectorStore::VectorStore(std::string index_path, std::string db_path)
    : impl_(std::make_unique<Impl>(std::move(index_path), std::move(db_path))) {}

VectorStore::~VectorStore() = default;

bool VectorStore::available() const { return impl_ && impl_->db != nullptr; }

void VectorStore::add_units(const std::vector<CodeUnit>& units,
                             const std::vector<std::vector<float>>& embeddings) {
    if (!available() || units.size() != embeddings.size() || units.empty()) return;

    impl_->ensure_index(static_cast<int64_t>(embeddings.front().size()));

    char* err = nullptr;
    sqlite3_exec(impl_->db, "BEGIN TRANSACTION;", nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); return; }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO rag_units (faiss_id, file_path, unit_name, content, removed) "
        "VALUES (?, ?, ?, ?, 0);";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    for (size_t i = 0; i < units.size(); ++i) {
        if (static_cast<int64_t>(embeddings[i].size()) != impl_->dim) continue;  // dimension mismatch, skip

        const int64_t id = impl_->next_faiss_id++;
        impl_->index->add_with_ids(1, embeddings[i].data(), &id);

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_text(stmt, 2, units[i].file_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, units[i].unit_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, units[i].content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

void VectorStore::remove_file(const std::string& file_path) {
    if (!available()) return;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "UPDATE rag_units SET removed = 1 WHERE file_path = ?;", -1,
                            &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM rag_index_state WHERE file_path = ?;", -1,
                            &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    impl_->maybe_rebuild();
}

std::vector<CodeUnit> VectorStore::search(const std::vector<float>& query, int top_k) const {
    std::vector<CodeUnit> results;
    if (!available() || !impl_->index || impl_->index->ntotal == 0 || top_k <= 0) return results;
    if (static_cast<int64_t>(query.size()) != impl_->dim) return results;

    // Over-fetch to leave room for filtering out tombstoned/deleted rows
    // that still physically live in the FAISS index until the next rebuild.
    const int64_t fetch_k =
        std::min<int64_t>(impl_->index->ntotal, static_cast<int64_t>(top_k) * 4 + 8);

    std::vector<float> distances(static_cast<size_t>(fetch_k));
    std::vector<int64_t> labels(static_cast<size_t>(fetch_k));
    impl_->index->search(1, query.data(), fetch_k, distances.data(), labels.data());

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT file_path, unit_name, content FROM rag_units WHERE faiss_id = ? AND removed = 0;";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    for (int64_t i = 0; i < fetch_k && static_cast<int>(results.size()) < top_k; ++i) {
        if (labels[static_cast<size_t>(i)] < 0) continue;  // FAISS pads short result lists with -1

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, labels[static_cast<size_t>(i)]);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            CodeUnit unit;
            unit.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            unit.unit_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            unit.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            results.push_back(std::move(unit));
        }
    }
    sqlite3_finalize(stmt);
    return results;
}

std::string VectorStore::get_indexed_hash(const std::string& file_path) const {
    if (!available()) return {};

    std::string hash;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT content_hash FROM rag_index_state WHERE file_path = ?;";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            hash = text ? reinterpret_cast<const char*>(text) : "";
        }
        sqlite3_finalize(stmt);
    }
    return hash;
}

void VectorStore::set_indexed_hash(const std::string& file_path, const std::string& content_hash) {
    if (!available()) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO rag_index_state (file_path, content_hash, indexed_at) "
        "VALUES (?, ?, datetime('now')) "
        "ON CONFLICT(file_path) DO UPDATE SET content_hash = excluded.content_hash, "
        "indexed_at = excluded.indexed_at;";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, content_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<std::string> VectorStore::indexed_file_paths() const {
    std::vector<std::string> paths;
    if (!available()) return paths;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "SELECT file_path FROM rag_index_state;", -1, &stmt,
                            nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) paths.emplace_back(reinterpret_cast<const char*>(text));
        }
        sqlite3_finalize(stmt);
    }
    return paths;
}

void VectorStore::save() const {
    if (!available()) return;
    impl_->save();
}

}  // namespace mygit::rag
