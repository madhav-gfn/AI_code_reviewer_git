#include "commands/review_command.h"

#include <exception>
#include <iostream>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "ai/review_aggregator.h"
#include "config/mygit_config.h"
#include "database/async_db_writer.h"
#include "database/sqlite_manager.h"
#include "decision_engine/decision_engine.h"
#include "diff_filter/diff_filter.h"
#include "git/git_diff.h"
#include "git/git_status.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "ui/terminal_ui.h"

namespace mygit::commands {

namespace {

// Above this many remaining files, batching would cost too many separate
// inference calls; fall back to one prompt built from the concatenated diff.
constexpr size_t kMaxBatchedFiles = 15;

std::string display_path(const git::FileDiff& fd) {
    return fd.new_path.empty() ? fd.old_path : fd.new_path;
}

// Loads the model into `owner` on first use and reuses it afterward — lets
// callers stay agnostic to whether the model was already loaded elsewhere
// (e.g. by an earlier cache miss in the same run).
ai::LlamaClient& get_or_create_llama(std::unique_ptr<ai::LlamaClient>& owner,
                                      const config::MygitConfig& cfg) {
    if (!owner) {
        owner = std::make_unique<ai::LlamaClient>(cfg.model_path, cfg.gpu_layers);
    }
    return *owner;
}

// Reviews `filtered.kept_files`. Each file's diff is fingerprinted
// (database::hash_diff_content) and looked up in `cache` first, so
// re-reviewing a diff that's byte-for-byte unchanged since last time reuses
// the stored result instead of re-running inference. The model is loaded
// lazily into `llama_owner` on the first actual cache miss, so a fully cached
// diff never pays the model-load cost at all.
//
// Uses one file-per-call batch when there's more than one file (Level 1
// batched per-file processing), a single direct call for exactly one file, or
// the old concatenated-diff prompt once the file count exceeds
// kMaxBatchedFiles. The batch path uses split prompts so the shared
// system-prompt prefix is only evaluated once (Level 1 prefix KV caching)
// instead of once per file.
ReviewResult run_batched_review(std::unique_ptr<ai::LlamaClient>& llama_owner,
                                 const config::MygitConfig& cfg,
                                 const ai::PromptBuilder& prompt_builder,
                                 const diff_filter::FilteredDiff& filtered,
                                 database::SqliteManager& cache) {
    const parsers::JsonParser parser;

    if (filtered.kept_files.size() == 1) {
        const git::FileDiff& fd = filtered.kept_files.front();
        const std::string path = display_path(fd);
        const std::string hash = database::hash_diff_content(path, fd.patch);

        ReviewResult cached;
        if (cache.get_cached_file_review(hash, cached)) {
            return cached;
        }

        const std::string prompt = prompt_builder.build_single_file_review_prompt(path, fd.patch);
        ReviewResult result =
            parser.parse_review(get_or_create_llama(llama_owner, cfg).review(prompt));
        cache.save_cached_file_review(hash, result);
        return result;
    }

    if (filtered.kept_files.size() > kMaxBatchedFiles) {
        const std::string hash = database::hash_diff_content("__batch__", filtered.patch_text);

        ReviewResult cached;
        if (cache.get_cached_file_review(hash, cached)) {
            return cached;
        }

        const std::string prompt = prompt_builder.build_review_prompt(filtered.patch_text);
        ReviewResult result =
            parser.parse_review(get_or_create_llama(llama_owner, cfg).review(prompt));
        cache.save_cached_file_review(hash, result);
        return result;
    }

    // Partition into cache hits and misses before touching the model, so a
    // diff that's entirely unchanged since the last review never loads it.
    std::vector<ReviewResult> per_file;
    per_file.reserve(filtered.kept_files.size());
    std::vector<std::pair<const git::FileDiff*, std::string>> misses;
    for (const git::FileDiff& fd : filtered.kept_files) {
        const std::string path = display_path(fd);
        const std::string hash = database::hash_diff_content(path, fd.patch);

        ReviewResult cached;
        if (cache.get_cached_file_review(hash, cached)) {
            per_file.push_back(std::move(cached));
        } else {
            misses.emplace_back(&fd, hash);
        }
    }

    if (!misses.empty()) {
        ai::LlamaClient& llama_client = get_or_create_llama(llama_owner, cfg);
        llama_client.cache_system_prefix(
            prompt_builder.build_single_file_review_prompt_split("", "").prefix);
        for (const auto& [fd_ptr, hash] : misses) {
            const ai::SplitPrompt prompt = prompt_builder.build_single_file_review_prompt_split(
                display_path(*fd_ptr), fd_ptr->patch);
            ReviewResult result = parser.parse_review(llama_client.review(prompt));
            cache.save_cached_file_review(hash, result);
            per_file.push_back(std::move(result));
        }
    }

    return ai::aggregate_reviews(per_file);
}

}  // namespace

int run_review() {
    config::MygitConfig cfg;
    try {
        cfg = config::load_config();
    } catch (const std::exception& e) {
        std::cerr << "\n  " << e.what() << "\n\n";
        return 1;
    }

    const git::GitDiff diff_provider;
    const std::vector<git::FileDiff> file_diffs = diff_provider.get_staged_file_diffs();
    if (file_diffs.empty()) {
        std::cout << "\n  No staged changes to review.\n\n";
        return 0;
    }

    const diff_filter::FilteredDiff filtered = diff_filter::filter(file_diffs);
    if (filtered.rejected) {
        std::cerr << "\n  " << filtered.reject_reason << "\n\n";
        return 1;
    }
    if (!filtered.skipped.empty()) {
        std::cout << "\n  Skipped " << filtered.skipped.size() << " file(s):\n";
        for (const std::string& s : filtered.skipped) {
            std::cout << "    - " << s << "\n";
        }
    }
    if (filtered.kept_files.empty()) {
        std::cout << "\n  Nothing left to review after filtering.\n\n";
        return 0;
    }

    const ai::PromptBuilder prompt_builder;

    const std::string db_path = (config::get_config_dir() / "mygit.db").string();

    // Constructed early so its background worker thread is up and ready by
    // the time inference finishes (bottleneck #2 — async DB writes).
    database::AsyncDbWriter db_writer(db_path);

    // Separate synchronous connection for the diff-content review cache —
    // cache lookups must happen before deciding whether to load the model at
    // all, so they can't go through the async writer.
    database::SqliteManager review_cache(db_path);

    ReviewResult result;
    long long inference_ms = 0;
    std::unique_ptr<ai::LlamaClient> llama_owner;
    {
        ui::Spinner spinner("Reviewing staged changes...");
        auto start = std::chrono::steady_clock::now();
        try {
            result = run_batched_review(llama_owner, cfg, prompt_builder, filtered, review_cache);
        } catch (const std::exception& e) {
            spinner.stop();
            std::cerr << "\n  AI review failed: " << e.what() << "\n\n";
            return 1;
        }
        auto end = std::chrono::steady_clock::now();
        inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }  // spinner stops and clears here

    ui::print_report(result);

    logger::log_review(result, result.safe, "review", inference_ms);

    // Persist to SQLite memory system without blocking the CLI's exit.
    const git::GitStatus status;
    db_writer.save_review_async(result, status.get_current_branch(), !result.safe);

    return result.safe ? 0 : 1;
}

}  // namespace mygit::commands
