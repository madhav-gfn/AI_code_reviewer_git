#pragma once

#include <string>

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

private:
    std::string model_path_;
    int n_gpu_layers_;
    int n_ctx_;
    void* model_ = nullptr;  // llama_model*
    void* ctx_ = nullptr;    // llama_context*
};

}  // namespace mygit::ai
