#include "ai/llama_client.h"

#include <llama.h>

#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace mygit::ai {

namespace {

// Forces the model's output to exactly match the FR-5 review JSON schema:
//   {"safe": bool, "issues": [{"severity": "...", "file": "...", "line": N, "message": "..."}]}
//
// This is the reliability lever: instead of asking nicely in the prompt and
// hoping the model returns valid JSON, the grammar makes invalid output
// structurally impossible. json_parser.cpp can trust its input.
constexpr const char* kReviewGrammar = R"GBNF(
root        ::= "{" ws "\"safe\":" ws boolean "," ws "\"issues\":" ws issue-array ws "}" ws
boolean     ::= ("true" | "false") ws
issue-array ::= "[" ws ( issue ("," ws issue)* )? "]" ws
issue       ::= "{" ws "\"severity\":" ws severity "," ws "\"file\":" ws string "," ws "\"line\":" ws number "," ws "\"message\":" ws string ws "}" ws
severity    ::= ("\"critical\"" | "\"high\"" | "\"medium\"" | "\"low\"") ws
string      ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4}) )* "\"" ws
number      ::= ("-"? ([0-9] | [1-9] [0-9]{0,8})) ws
ws          ::= | " " | "\n" [ \t]{0,20}
)GBNF";

std::string resolve_model_path(std::string model_path) {
    if (!model_path.empty()) {
        return model_path;
    }
    if (const char* env = std::getenv("MYGIT_MODEL_PATH")) {
        return env;
    }
    return "models/model.gguf";
}

int resolve_gpu_layers(int n_gpu_layers) {
    if (n_gpu_layers >= 0) {
        return n_gpu_layers;
    }
    if (const char* env = std::getenv("MYGIT_GPU_LAYERS")) {
        try {
            return std::stoi(env);
        } catch (...) {
            // fall through to CPU-only default
        }
    }
    return 0;
}

void ensure_backend_initialized() {
    static bool initialized = false;
    if (!initialized) {
        ggml_backend_load_all();
        llama_backend_init();
        initialized = true;
    }
}

}  // namespace

LlamaClient::LlamaClient(std::string model_path, int n_gpu_layers)
    : model_path_(resolve_model_path(std::move(model_path))),
      n_gpu_layers_(resolve_gpu_layers(n_gpu_layers)),
      n_ctx_(4096) {
    ensure_backend_initialized();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = n_gpu_layers_;

    llama_model* model = llama_model_load_from_file(model_path_.c_str(), model_params);
    if (model == nullptr) {
        throw std::runtime_error(
            "Failed to load model: " + model_path_ +
            " (set MYGIT_MODEL_PATH, or drop a .gguf at models/model.gguf)");
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = n_ctx_;
    ctx_params.n_batch = n_ctx_;

    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (ctx == nullptr) {
        llama_model_free(model);
        throw std::runtime_error("Failed to create llama context for model: " + model_path_);
    }

    model_ = model;
    ctx_ = ctx;
}

LlamaClient::~LlamaClient() {
    if (ctx_ != nullptr) {
        llama_free(static_cast<llama_context*>(ctx_));
    }
    if (model_ != nullptr) {
        llama_model_free(static_cast<llama_model*>(model_));
    }
}

std::string LlamaClient::review(const std::string& prompt) const {
    auto* model = static_cast<llama_model*>(model_);
    auto* ctx = static_cast<llama_context*>(ctx_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    // Reset KV cache state so this instance can safely be reused across
    // multiple review() calls (a fresh CLI process only needs this once,
    // but it keeps the class correct if reused, e.g. by a future
    // multi-agent driver running several specialist passes per diff).
    llama_memory_clear(llama_get_memory(ctx), /*data=*/true);

    // Apply the model's own embedded chat template so the prompt is
    // formatted the way the model was instruction-tuned to expect.
    llama_chat_message msg{"user", prompt.c_str()};
    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);

    std::string formatted;
    if (tmpl != nullptr) {
        std::vector<char> buf(prompt.size() * 2 + 256);
        int32_t needed = llama_chat_apply_template(tmpl, &msg, 1, /*add_ass=*/true, buf.data(),
                                                     static_cast<int32_t>(buf.size()));
        if (needed > static_cast<int32_t>(buf.size())) {
            buf.resize(needed);
            needed = llama_chat_apply_template(tmpl, &msg, 1, true, buf.data(), needed);
        }
        if (needed > 0) {
            formatted.assign(buf.data(), needed);
        }
    }
    if (formatted.empty()) {
        // No embedded chat template in this GGUF - fall back to a raw prompt.
        formatted = prompt;
    }

    // Tokenize.
    const int n_prompt_tokens = -llama_tokenize(
        vocab, formatted.c_str(), static_cast<int32_t>(formatted.size()), nullptr, 0, true, true);
    std::vector<llama_token> prompt_tokens(n_prompt_tokens);
    if (llama_tokenize(vocab, formatted.c_str(), static_cast<int32_t>(formatted.size()),
                        prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()), true,
                        true) < 0) {
        throw std::runtime_error("Failed to tokenize prompt");
    }
    if (static_cast<int>(prompt_tokens.size()) >= n_ctx_) {
        throw std::runtime_error(
            "Diff is too large for the model's context window (n_ctx=" +
            std::to_string(n_ctx_) + "); consider chunking the diff per file.");
    }

    // Deterministic, grammar-constrained sampler: the grammar guarantees
    // structurally valid JSON, greedy sampling keeps a code reviewer's
    // verdict reproducible run to run.
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_grammar(vocab, kReviewGrammar, "root"));
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    llama_batch batch =
        llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));

    std::string output;
    const int max_response_tokens = 1024;
    int n_decoded = 0;
    int n_pos = 0;

    while (n_pos + batch.n_tokens < n_ctx_ && n_decoded < max_response_tokens) {
        if (llama_decode(ctx, batch)) {
            llama_sampler_free(sampler);
            throw std::runtime_error("llama_decode failed");
        }
        n_pos += batch.n_tokens;

        llama_token new_token = llama_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, new_token)) {
            break;
        }

        char piece[256];
        const int n = llama_token_to_piece(vocab, new_token, piece, sizeof(piece), 0, true);
        if (n > 0) {
            output.append(piece, n);
        }

        batch = llama_batch_get_one(&new_token, 1);
        ++n_decoded;
    }

    llama_sampler_free(sampler);
    return output;
}

}  // namespace mygit::ai
