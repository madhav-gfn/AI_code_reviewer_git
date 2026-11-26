#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mygit::rag {

// Wraps an ONNX Runtime session for a text embedding model (e.g.
// Qwen2.5-Coder-1.5B / Qodo-Embed exported to ONNX) plus the BpeTokenizer
// needed to feed it. Used to embed both indexed CodeUnits (ingestion,
// RagOrchestrator::update_index) and the diff text being committed (query,
// RagOrchestrator::get_context_for_diff).
//
// onnxruntime_cxx_api.h is intentionally kept out of this header (Pimpl)
// per the project's header-hygiene rule - only embedder.cpp depends on it.
//
// Graceful fallback (project rule): if the model or tokenizer file is
// missing/unreadable, or ONNX Runtime fails to create a session, the
// Embedder simply reports available() == false and logs a warning once, at
// construction. It never throws. Callers (RagOrchestrator) must check
// available() before calling embed().
//
// Thread-safety: a single Ort::Session supports concurrent Run() calls, so
// embed() may be called from multiple threads on the same Embedder
// instance - RagOrchestrator::update_index() relies on this to embed
// several CodeUnits in parallel during ingestion.
class Embedder {
public:
    // model_path/tokenizer_path default to the paths documented in
    // docs/architecture_review.md. Neither ships with the repo; until both
    // are present, available() is false and the RAG pipeline stays in
    // zero-RAG fallback mode.
    explicit Embedder(std::string model_path = "models/embedding_model.onnx",
                       std::string tokenizer_path = "models/tokenizer.json");
    ~Embedder();

    Embedder(const Embedder&) = delete;
    Embedder& operator=(const Embedder&) = delete;

    bool available() const;

    // Returns a mean-pooled, L2-normalized embedding, or std::nullopt if
    // the embedder isn't available or inference fails for this input.
    std::optional<std::vector<float>> embed(const std::string& text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mygit::rag
