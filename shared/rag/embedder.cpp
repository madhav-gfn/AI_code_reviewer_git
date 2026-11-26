#include "rag/embedder.h"

#include "rag/bpe_tokenizer.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>

#include <onnxruntime_cxx_api.h>

namespace mygit::rag {

namespace {
// Truncates very long diffs/units so a single embed() call stays bounded -
// mirrors kMaxBatchedFiles-style guards elsewhere in the pipeline rather
// than letting an outlier diff blow up inference latency.
constexpr int64_t kMaxTokens = 512;
}  // namespace

struct Embedder::Impl {
    std::string model_path;
    std::string tokenizer_path;

    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "mygit-rag"};
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<BpeTokenizer> tokenizer;

    std::string input_ids_name;
    std::string attention_mask_name;
    std::string token_type_ids_name;  // left empty if the model doesn't take one
    std::string output_name;

    bool ready = false;

    Impl(std::string mp, std::string tp)
        : model_path(std::move(mp)), tokenizer_path(std::move(tp)) {
        tokenizer = BpeTokenizer::load(tokenizer_path);
        if (!tokenizer) {
            std::cerr << "[rag] tokenizer unavailable (" << tokenizer_path
                      << ") - repository context retrieval disabled, falling back to "
                         "diff-only prompts.\n";
            return;
        }

        if (!std::filesystem::exists(model_path)) {
            std::cerr << "[rag] embedding model not found (" << model_path
                      << ") - repository context retrieval disabled, falling back to "
                         "diff-only prompts.\n";
            return;
        }

        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Best-effort CUDA execution provider. Only compiled in when the
        // project is built with -DMYGIT_ENABLE_ONNX_CUDA=ON *and* the vcpkg
        // 'onnx-cuda' manifest feature (which additionally needs cuDNN
        // installed locally - vcpkg can't fetch that itself, see
        // CMakeLists.txt). Without it, this is a no-op and we go straight
        // to the CPU provider below.
#ifdef MYGIT_ONNXRUNTIME_CUDA
        try {
            OrtCUDAProviderOptions cuda_options{};
            options.AppendExecutionProvider_CUDA(cuda_options);
        } catch (const std::exception& e) {
            std::cerr << "[rag] CUDA execution provider unavailable (" << e.what()
                      << "), using CPU for embeddings.\n";
        }
#endif

        try {
#ifdef _WIN32
            const std::wstring wpath(model_path.begin(), model_path.end());
            session = std::make_unique<Ort::Session>(env, wpath.c_str(), options);
#else
            session = std::make_unique<Ort::Session>(env, model_path.c_str(), options);
#endif
        } catch (const std::exception& e) {
            std::cerr << "[rag] failed to load embedding model (" << e.what()
                      << ") - falling back to diff-only prompts.\n";
            return;
        }

        if (!resolve_io_names()) {
            std::cerr << "[rag] embedding model has an unexpected input/output signature - "
                         "falling back to diff-only prompts.\n";
            session.reset();
            return;
        }

        ready = true;
    }

    // Model I/O names aren't hardcoded (we don't ship the model, so we
    // don't get to assume an export convention) - instead they're resolved
    // from the session's actual graph signature at load time.
    bool resolve_io_names() {
        Ort::AllocatorWithDefaultOptions alloc;

        const size_t n_inputs = session->GetInputCount();
        for (size_t i = 0; i < n_inputs; ++i) {
            auto name = session->GetInputNameAllocated(i, alloc);
            const std::string n = name.get();
            if (n.find("input_ids") != std::string::npos) {
                input_ids_name = n;
            } else if (n.find("attention_mask") != std::string::npos) {
                attention_mask_name = n;
            } else if (n.find("token_type_ids") != std::string::npos) {
                token_type_ids_name = n;
            }
        }
        if (input_ids_name.empty()) return false;

        if (session->GetOutputCount() == 0) return false;
        auto out_name = session->GetOutputNameAllocated(0, alloc);
        output_name = out_name.get();
        return true;
    }
};

Embedder::Embedder(std::string model_path, std::string tokenizer_path)
    : impl_(std::make_unique<Impl>(std::move(model_path), std::move(tokenizer_path))) {}

Embedder::~Embedder() = default;

bool Embedder::available() const { return impl_ && impl_->ready; }

std::optional<std::vector<float>> Embedder::embed(const std::string& text) const {
    if (!available()) return std::nullopt;

    try {
        std::vector<int32_t> ids32 = impl_->tokenizer->encode(text);
        if (ids32.empty()) return std::nullopt;
        if (static_cast<int64_t>(ids32.size()) > kMaxTokens) {
            ids32.resize(static_cast<size_t>(kMaxTokens));
        }

        const int64_t seq_len = static_cast<int64_t>(ids32.size());
        const std::vector<int64_t> input_ids(ids32.begin(), ids32.end());
        const std::vector<int64_t> attention_mask(static_cast<size_t>(seq_len), 1);
        const std::vector<int64_t> token_type_ids(static_cast<size_t>(seq_len), 0);
        const std::array<int64_t, 2> shape{1, seq_len};

        const Ort::MemoryInfo mem_info =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<Ort::Value> inputs;
        std::vector<const char*> input_names;

        inputs.push_back(Ort::Value::CreateTensor<int64_t>(
            mem_info, const_cast<int64_t*>(input_ids.data()), input_ids.size(), shape.data(),
            shape.size()));
        input_names.push_back(impl_->input_ids_name.c_str());

        if (!impl_->attention_mask_name.empty()) {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                mem_info, const_cast<int64_t*>(attention_mask.data()), attention_mask.size(),
                shape.data(), shape.size()));
            input_names.push_back(impl_->attention_mask_name.c_str());
        }
        if (!impl_->token_type_ids_name.empty()) {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                mem_info, const_cast<int64_t*>(token_type_ids.data()), token_type_ids.size(),
                shape.data(), shape.size()));
            input_names.push_back(impl_->token_type_ids_name.c_str());
        }

        const char* output_names[] = {impl_->output_name.c_str()};

        auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, input_names.data(),
                                            inputs.data(), inputs.size(), output_names, 1);
        if (outputs.empty() || !outputs[0].IsTensor()) return std::nullopt;

        const Ort::Value& out = outputs[0];
        const auto out_shape = out.GetTensorTypeAndShapeInfo().GetShape();
        const float* data = out.GetTensorData<float>();
        const size_t total = out.GetTensorTypeAndShapeInfo().GetElementCount();

        std::vector<float> pooled;
        if (out_shape.size() == 3) {
            // [batch, seq, hidden] per-token embeddings - mean-pool over the
            // sequence dimension (attention_mask is all-ones here since we
            // don't pad, so a plain average is equivalent to a masked one).
            const int64_t hidden = out_shape[2];
            const int64_t pooled_len = std::min(seq_len, out_shape[1]);
            pooled.assign(static_cast<size_t>(hidden), 0.0f);
            for (int64_t t = 0; t < pooled_len; ++t) {
                for (int64_t h = 0; h < hidden; ++h) {
                    pooled[static_cast<size_t>(h)] +=
                        data[static_cast<size_t>(t * hidden + h)];
                }
            }
            if (pooled_len > 0) {
                for (float& v : pooled) v /= static_cast<float>(pooled_len);
            }
        } else {
            // Already a single pooled vector, e.g. [batch, hidden] or [hidden].
            pooled.assign(data, data + total);
        }

        double norm = 0.0;
        for (float v : pooled) norm += static_cast<double>(v) * v;
        norm = std::sqrt(norm);
        if (norm > 1e-9) {
            for (float& v : pooled) v = static_cast<float>(static_cast<double>(v) / norm);
        }
        return pooled;
    } catch (const std::exception& e) {
        std::cerr << "[rag] embedding inference failed: " << e.what() << "\n";
        return std::nullopt;
    }
}

}  // namespace mygit::rag
