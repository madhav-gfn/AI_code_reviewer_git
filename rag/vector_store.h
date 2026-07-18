#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rag/code_parser.h"

namespace mygit::rag {

// Persistent nearest-neighbor index over CodeUnit embeddings, backing
// RagOrchestrator's retrieval step. Two things are persisted across runs:
//   - a FAISS IndexHNSWFlat of the embedding vectors (index_path). Lazily
//     created on the first add_units() call, since the embedding dimension
//     isn't known until then.
//   - a SQLite table mapping each FAISS id back to its CodeUnit, plus a
//     per-file "last indexed content hash" table used by RagOrchestrator
//     for incremental updates (db_path, tables rag_units / rag_index_state)
//
// faiss/*.h and sqlite3.h are intentionally kept out of this header (Pimpl)
// per the project's header-hygiene rule - only vector_store.cpp depends on
// them.
//
// Graceful fallback: if the SQLite metadata store can't be opened/created,
// the store reports available() == false and every method becomes a safe
// no-op (add_units/remove_file do nothing, search returns empty). It never
// throws.
class VectorStore {
public:
    VectorStore(std::string index_path, std::string db_path);
    ~VectorStore();

    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    bool available() const;

    // Adds `units[i]` with embedding `embeddings[i]` to the index and
    // metadata store. `units.size()` must equal `embeddings.size()`; a
    // mismatch is treated as a no-op. All embeddings must share the same
    // dimension as the index (its dimension is fixed by whichever call
    // creates it first).
    void add_units(const std::vector<CodeUnit>& units,
                    const std::vector<std::vector<float>>& embeddings);

    // Tombstones every unit previously indexed for `file_path` (excludes
    // them from search()) and clears its incremental-indexing hash.
    // IndexHNSWFlat has no native per-id removal, so accumulated
    // tombstones trigger a lazy rebuild (reconstruct surviving vectors into
    // a fresh graph) once they cross an internal threshold.
    void remove_file(const std::string& file_path);

    // Returns up to `top_k` CodeUnits whose embeddings are closest (cosine
    // similarity, via inner product on L2-normalized vectors) to `query`.
    // Returns an empty vector if the index doesn't exist yet or is empty.
    std::vector<CodeUnit> search(const std::vector<float>& query, int top_k) const;

    // Per-file incremental-indexing bookkeeping, used by
    // RagOrchestrator::update_index() to skip files that haven't changed
    // since they were last indexed. Returns "" if `file_path` has never
    // been indexed.
    std::string get_indexed_hash(const std::string& file_path) const;
    void set_indexed_hash(const std::string& file_path, const std::string& content_hash);

    // Returns every file_path that currently has an indexing-state entry,
    // i.e. every file RagOrchestrator has indexed at some point. Used to
    // detect files that were indexed previously but are no longer tracked
    // by git (deleted/renamed away), so they can be purged via
    // remove_file().
    std::vector<std::string> indexed_file_paths() const;

    // Flushes the FAISS index to `index_path`. SQLite metadata is written
    // transactionally as it changes and doesn't need an explicit flush.
    void save() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mygit::rag
