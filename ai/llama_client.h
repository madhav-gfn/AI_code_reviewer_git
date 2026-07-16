#pragma once

#include <string>

#include "ai/prompt_builder.h"

namespace mygit::ai {

// Wraps a local llama.cpp model for code review inference (FR-3, NFR Privacy).
// Loads a GGUF model once at construction and reuses it for each review()
// call. All inference runs locally - no network calls.
class LlamaClient {
public:
    // model_path: path to a GGUF model file. If empty, resolved from the
    // MYGIT_MODEL_PATH env var, falling back to "models/model.gguf".
    //
    // n_gpu_layers: number of model layers to offload to GPU. -1 (default)
    // resolves from the MYGIT_GPU_LAYERS env var, falling back to 0 (CPU
    // only). Set MYGIT_GPU_LAYERS=999 once a CUDA-enabled build is set up
    // to offload the whole model.
    explicit LlamaClient(std::string model_path = {}, int n_gpu_layers = -1);
    ~LlamaClient();

    LlamaClient(const LlamaClient&) = delete;
    LlamaClient& operator=(const LlamaClient&) = delete;

    // Runs the model on `prompt` and returns its raw text response. The
    // response is grammar-constrained to the FR-5 JSON schema (see
    // kReviewGrammar in llama_client.cpp), so it is guaranteed to parse -
    // no prompt-engineering-and-hope required.
    std::string review(const std::string& prompt) const;

    // Runs the model on `prompt` to generate a conventional commit message.
    // No grammar constraints — the prompt instructs the model to return only
    // the commit message string.
    std::string generate_commit_message(const std::string& prompt) const;

    // Split-prompt variants (Level 1 prefix KV caching). When `prompt.prefix`
    // matches the currently cached prefix (see cache_system_prefix()), only
    // `prompt.suffix` is tokenized and decoded — the prefix's KV state is
    // reused as-is. Otherwise the prefix is (re-)evaluated and cached before
    // the suffix is decoded. Ideal for reviewing several files in a batch,
    // where every call shares the same system-prompt prefix.
    std::string review(const SplitPrompt& prompt) const;
    std::string generate_commit_message(const SplitPrompt& prompt) const;

    // Pre-evaluates and caches the KV state for `prefix` so the first
    // subsequent split-prompt call with a matching prefix skips prefix
    // evaluation entirely. Purely an optimization — calling it is optional,
    // since review(SplitPrompt)/generate_commit_message(SplitPrompt) cache
    // the prefix themselves on first use; call it upfront (e.g. before a
    // per-file review loop) to move that cost out of the first file's timing.
    void cache_system_prefix(const std::string& prefix);

private:
    // Ensures `formatted_prefix` (already chat-template formatted) is
    // present in the KV cache as a clean prefix — reusing the cached state
    // if it matches what's already there, or clearing and re-decoding it
    // otherwise. Returns the prefix length in tokens, i.e. the position at
    // which the caller should decode its suffix tokens.
    int ensure_prefix_cached(const std::string& formatted_prefix) const;

    std::string model_path_;
    int n_gpu_layers_;
    int n_ctx_;
    void* model_ = nullptr;  // llama_model*
    void* ctx_ = nullptr;    // llama_context*

    mutable std::string cached_prefix_text_;
    mutable int cached_prefix_len_ = 0;  // token count of the cached prefix
    mutable bool prefix_cached_ = false;
};

}  // namespace mygit::ai
