#pragma once

#include <memory>
#include <string>

namespace mygit::rag {

// Ties CodeParser + Embedder + VectorStore together into the RAG pipeline
// described in docs/architecture_review.md bottleneck #3: parse the repo
// into CodeUnits, embed them, index them, and retrieve the top-k most
// relevant ones for a given diff so commit-message generation isn't
// operating blind on `+`/`-` lines alone.
//
// Graceful fallback (project rule): construction never throws. If the
// embedding model/tokenizer or the vector index can't be loaded,
// available() is false and update_index()/get_context_for_diff() become
// safe no-ops (the latter returning ""), so callers can use this
// unconditionally and simply get zero-RAG behavior back if the pipeline
// isn't set up yet.
class RagOrchestrator {
public:
    // db_path/index_path: where the FAISS index + its SQLite metadata live
    // (VectorStore, sharing the project's existing mygit.db by convention -
    // pass the same path used for database::SqliteManager/AsyncDbWriter).
    // model_path/tokenizer_path: the ONNX embedding model + its
    // tokenizer.json (Embedder). All default to the locations documented in
    // the architecture review; neither ships with the repo, so available()
    // is false out of the box until both are supplied.
    explicit RagOrchestrator(std::string db_path = "logs/mygit.db",
                              std::string index_path = "logs/rag.index",
                              std::string model_path = "models/embedding_model.onnx",
                              std::string tokenizer_path = "models/tokenizer.json");
    ~RagOrchestrator();

    RagOrchestrator(const RagOrchestrator&) = delete;
    RagOrchestrator& operator=(const RagOrchestrator&) = delete;

    bool available() const;

    // Brings the index up to date with the current working tree: lists
    // git-tracked files (`git ls-files`), re-parses/re-embeds any whose
    // content has changed since they were last indexed (by content hash,
    // not commit - so staged-but-uncommitted edits are picked up too), and
    // purges files that are no longer tracked. Safe to call on every
    // `mygit commit` - already-up-to-date files are skipped cheaply. A
    // no-op if !available().
    void update_index();

    // Embeds `patch_text` and returns the top_k most relevant CodeUnits,
    // formatted as:
    //   --- Context from <file_path> ---
    //   <content>
    // (one block per unit, blank-line separated). Returns "" if
    // !available() or retrieval fails for any reason - callers should treat
    // that identically to "no context" (today's zero-RAG behavior).
    std::string get_context_for_diff(const std::string& patch_text, int top_k = 3) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mygit::rag
