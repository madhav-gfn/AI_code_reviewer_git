#include "ai/llama_client.h"

#include <llama.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
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
        // Silence ggml/llama diagnostic output that would otherwise spam stderr.
        llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
        ggml_backend_load_all();
        llama_backend_init();
        initialized = true;
    }
}

// Applies the model's own embedded chat template to `prompt`, formatting it
// the way the model was instruction-tuned to expect. `add_ass` controls
// whether the assistant-turn-start tokens are appended at the end — pass
// false when formatting a prefix that a suffix will still be appended to
// (see PromptBuilder::SplitPrompt), true for a complete, ready-to-generate
// prompt. Falls back to the raw prompt text if the GGUF has no template.
std::string apply_chat_template(const llama_model* model, const std::string& prompt, bool add_ass) {
    llama_chat_message msg{"user", prompt.c_str()};
    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);

    std::string formatted;
    if (tmpl != nullptr) {
        std::vector<char> buf(prompt.size() * 2 + 256);
        int32_t needed = llama_chat_apply_template(tmpl, &msg, 1, add_ass, buf.data(),
                                                     static_cast<int32_t>(buf.size()));
        if (needed > static_cast<int32_t>(buf.size())) {
            buf.resize(needed);
            needed = llama_chat_apply_template(tmpl, &msg, 1, add_ass, buf.data(), needed);
        }
        if (needed > 0) {
            formatted.assign(buf.data(), needed);
        }
    }
    if (formatted.empty()) {
        // No embedded chat template in this GGUF - fall back to a raw prompt.
        formatted = prompt;
    }
    return formatted;
}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text) {
    const int n_tokens = -llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                                          nullptr, 0, true, true);
    std::vector<llama_token> tokens(n_tokens);
    if (llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(),
                        static_cast<int32_t>(tokens.size()), true, true) < 0) {
        throw std::runtime_error("Failed to tokenize prompt");
    }
    return tokens;
}

// Runs the decode + sample generation loop starting at KV position
// `start_pos`, after `prefix_tokens_decoded + suffix_tokens.size()` tokens
// are (about to be) in the KV cache. Shared by review() and
// generate_commit_message() (both the plain-string and split-prompt forms).
std::string generate(llama_context* ctx, const llama_vocab* vocab, llama_sampler* sampler,
                      std::vector<llama_token>& suffix_tokens, int start_pos, int n_ctx,
                      int max_response_tokens) {
    if (start_pos + static_cast<int>(suffix_tokens.size()) >= n_ctx) {
        throw std::runtime_error(
            "Diff is too large for the model's context git (n_ctx=" + std::to_string(n_ctx) +
            "); consider chunking the diff per file.");
    }

    llama_batch batch =
        llama_batch_get_one(suffix_tokens.data(), static_cast<int32_t>(suffix_tokens.size()));

    std::string output;
    int n_decoded = 0;
    int n_pos = start_pos;

    while (n_pos + batch.n_tokens < n_ctx && n_decoded < max_response_tokens) {
        if (llama_decode(ctx, batch)) {
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

    return output;
}

std::string trim_to_first_line(const std::string& output) {
    auto start = output.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return {};
    }
    auto end = output.find_last_not_of(" \t\n\r");
    auto newline_pos = output.find('\n', start);
    if (newline_pos != std::string::npos && newline_pos <= end) {
        end = newline_pos - 1;
    }
    return output.substr(start, end - start + 1);
}

}  // namespace

LlamaClient::LlamaClient(std::string model_path, int n_gpu_layers)
    : model_path_(resolve_model_path(std::move(model_path))),
      n_gpu_layers_(resolve_gpu_layers(n_gpu_layers)),
      n_ctx_(8192) {
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

int LlamaClient::ensure_prefix_cached(const std::string& formatted_prefix) const {
    auto* ctx = static_cast<llama_context*>(ctx_);

    if (prefix_cached_ && cached_prefix_text_ == formatted_prefix) {
        // Already decoded — drop any suffix/generation tokens left over from
        // a previous call so the KV cache holds exactly the prefix again.
        llama_memory_seq_rm(llama_get_memory(ctx), 0, cached_prefix_len_, -1);
        return cached_prefix_len_;
    }

    auto* model = static_cast<llama_model*>(model_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    llama_memory_clear(llama_get_memory(ctx), /*data=*/true);

    std::vector<llama_token> tokens = tokenize(vocab, formatted_prefix);
    if (!tokens.empty()) {
        llama_batch batch =
            llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
        if (llama_decode(ctx, batch)) {
            prefix_cached_ = false;
            throw std::runtime_error("llama_decode failed while caching prompt prefix");
        }
    }

    cached_prefix_text_ = formatted_prefix;
    cached_prefix_len_ = static_cast<int>(tokens.size());
    prefix_cached_ = true;
    return cached_prefix_len_;
}

void LlamaClient::cache_system_prefix(const std::string& prefix) {
    auto* model = static_cast<llama_model*>(model_);
    const std::string formatted_prefix = apply_chat_template(model, prefix, /*add_ass=*/false);
    ensure_prefix_cached(formatted_prefix);
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
    prefix_cached_ = false;

    const std::string formatted = apply_chat_template(model, prompt, /*add_ass=*/true);
    std::vector<llama_token> prompt_tokens = tokenize(vocab, formatted);

    // Deterministic, grammar-constrained sampler: the grammar guarantees
    // structurally valid JSON, greedy sampling keeps a code reviewer's
    // verdict reproducible run to run.
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_grammar(vocab, kReviewGrammar, "root"));
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    std::string output;
    try {
        output = generate(ctx, vocab, sampler, prompt_tokens, /*start_pos=*/0, n_ctx_,
                           /*max_response_tokens=*/1024);
    } catch (...) {
        llama_sampler_free(sampler);
        throw;
    }
    llama_sampler_free(sampler);
    return output;
}

std::string LlamaClient::generate_commit_message(const std::string& prompt) const {
    auto* model = static_cast<llama_model*>(model_);
    auto* ctx = static_cast<llama_context*>(ctx_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    llama_memory_clear(llama_get_memory(ctx), /*data=*/true);
    prefix_cached_ = false;

    const std::string formatted = apply_chat_template(model, prompt, /*add_ass=*/true);
    std::vector<llama_token> prompt_tokens = tokenize(vocab, formatted);

    // No grammar constraint — just greedy sampling.
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    std::string output;
    try {
        output = generate(ctx, vocab, sampler, prompt_tokens, /*start_pos=*/0, n_ctx_,
                           /*max_response_tokens=*/128);
    } catch (...) {
        llama_sampler_free(sampler);
        throw;
    }
    llama_sampler_free(sampler);

    // Trim whitespace / extra lines — the model may emit leading/trailing
    // newlines or more than one line.
    return trim_to_first_line(output);
}

std::string LlamaClient::review(const SplitPrompt& prompt) const {
    auto* model = static_cast<llama_model*>(model_);
    auto* ctx = static_cast<llama_context*>(ctx_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    const std::string formatted_prefix = apply_chat_template(model, prompt.prefix, /*add_ass=*/false);
    std::vector<llama_token> prefix_tokens = tokenize(vocab, formatted_prefix);

    const std::string formatted_full =
        apply_chat_template(model, prompt.prefix + prompt.suffix, /*add_ass=*/true);
    std::vector<llama_token> full_tokens = tokenize(vocab, formatted_full);

    const bool prefix_matches =
        full_tokens.size() >= prefix_tokens.size() &&
        std::equal(prefix_tokens.begin(), prefix_tokens.end(), full_tokens.begin());

    auto t_prefix_start = std::chrono::steady_clock::now();
    int start_pos = 0;
    std::vector<llama_token> suffix_tokens;
    if (prefix_matches) {
        start_pos = ensure_prefix_cached(formatted_prefix);
        suffix_tokens.assign(full_tokens.begin() + prefix_tokens.size(), full_tokens.end());
    } else {
        // Tokenization merged across the prefix/suffix boundary differently
        // than expected — safest fallback is to decode everything fresh.
        llama_memory_clear(llama_get_memory(ctx), /*data=*/true);
        prefix_cached_ = false;
        suffix_tokens = full_tokens;
        start_pos = 0;
    }
    auto t_prefix_end = std::chrono::steady_clock::now();

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_grammar(vocab, kReviewGrammar, "root"));
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    std::string output;
    auto t_suffix_start = std::chrono::steady_clock::now();
    try {
        output = generate(ctx, vocab, sampler, suffix_tokens, start_pos, n_ctx_,
                           /*max_response_tokens=*/1024);
    } catch (...) {
        llama_sampler_free(sampler);
        throw;
    }
    llama_sampler_free(sampler);
    auto t_suffix_end = std::chrono::steady_clock::now();

    spdlog::info(
        "llama_client: prefix_eval_ms={} suffix_eval_ms={} prefix_tokens={} suffix_tokens={} "
        "prefix_cache_hit={}",
        std::chrono::duration_cast<std::chrono::milliseconds>(t_prefix_end - t_prefix_start).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(t_suffix_end - t_suffix_start).count(),
        prefix_tokens.size(), suffix_tokens.size(), prefix_matches);

    return output;
}

std::string LlamaClient::generate_commit_message(const SplitPrompt& prompt) const {
    auto* model = static_cast<llama_model*>(model_);
    auto* ctx = static_cast<llama_context*>(ctx_);
    const llama_vocab* vocab = llama_model_get_vocab(model);

    const std::string formatted_prefix = apply_chat_template(model, prompt.prefix, /*add_ass=*/false);
    std::vector<llama_token> prefix_tokens = tokenize(vocab, formatted_prefix);

    const std::string formatted_full =
        apply_chat_template(model, prompt.prefix + prompt.suffix, /*add_ass=*/true);
    std::vector<llama_token> full_tokens = tokenize(vocab, formatted_full);

    const bool prefix_matches =
        full_tokens.size() >= prefix_tokens.size() &&
        std::equal(prefix_tokens.begin(), prefix_tokens.end(), full_tokens.begin());

    auto t_prefix_start = std::chrono::steady_clock::now();
    int start_pos = 0;
    std::vector<llama_token> suffix_tokens;
    if (prefix_matches) {
        start_pos = ensure_prefix_cached(formatted_prefix);
        suffix_tokens.assign(full_tokens.begin() + prefix_tokens.size(), full_tokens.end());
    } else {
        llama_memory_clear(llama_get_memory(ctx), /*data=*/true);
        prefix_cached_ = false;
        suffix_tokens = full_tokens;
        start_pos = 0;
    }
    auto t_prefix_end = std::chrono::steady_clock::now();

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    std::string output;
    auto t_suffix_start = std::chrono::steady_clock::now();
    try {
        output = generate(ctx, vocab, sampler, suffix_tokens, start_pos, n_ctx_,
                           /*max_response_tokens=*/128);
    } catch (...) {
        llama_sampler_free(sampler);
        throw;
    }
    llama_sampler_free(sampler);
    auto t_suffix_end = std::chrono::steady_clock::now();

    spdlog::info(
        "llama_client: prefix_eval_ms={} suffix_eval_ms={} prefix_tokens={} suffix_tokens={} "
        "prefix_cache_hit={}",
        std::chrono::duration_cast<std::chrono::milliseconds>(t_prefix_end - t_prefix_start).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(t_suffix_end - t_suffix_start).count(),
        prefix_tokens.size(), suffix_tokens.size(), prefix_matches);

    return trim_to_first_line(output);
}

}  // namespace mygit::ai
