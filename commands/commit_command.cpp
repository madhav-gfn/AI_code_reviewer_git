#include "commands/commit_command.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <chrono>
#include <memory>
#include <utility>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "ai/review_aggregator.h"
#include "config/mygit_config.h"
#include "database/async_db_writer.h"
#include "database/sqlite_manager.h"
#include "decision_engine/decision_engine.h"
#include "diff_filter/diff_filter.h"
#include "git/git_diff.h"
#include "git/git_runner.h"
#include "git/git_status.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "ui/terminal_ui.h"

#include <filesystem>

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

// Asks the user "[Y/n/e to edit]" and returns the lowercase first character
// of their response, defaulting to 'y' on empty input.
char prompt_user_choice() {
    std::cout << "\n  Use this? [Y/n/e to edit]: " << std::flush;
    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return 'y';
    return static_cast<char>(std::tolower(static_cast<unsigned char>(input[0])));
}

// Opens $EDITOR (or notepad/vi as fallback) with a temp file containing
// `initial_content`, waits for the editor to close, and returns the
// file contents. Returns empty string on failure.
std::string open_editor_with(const std::string& initial_content) {
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto tmp_path = tmp_dir / "mygit_commit_msg.txt";

    // Write initial content.
    {
        std::ofstream f(tmp_path);
        if (!f) return {};
        f << initial_content;
    }

    // Determine editor.
    std::string editor;
    if (const char* env = std::getenv("EDITOR")) {
        editor = env;
    } else if (const char* vis = std::getenv("VISUAL")) {
        editor = vis;
    } else {
#if defined(_WIN32)
        editor = "notepad";
#else
        editor = "vi";
#endif
    }

    const std::string cmd = editor + " \"" + tmp_path.string() + "\"";
    std::system(cmd.c_str());

    // Read back.
    std::ifstream f(tmp_path);
    if (!f) return {};
    std::string result((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // Trim trailing whitespace.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

    std::filesystem::remove(tmp_path);
    return result;
}

}  // namespace

int run_commit(const std::string& message) {
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
        std::cout << "\n  Nothing staged to commit.\n\n";
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
        std::cout << "\n  Nothing left to commit after filtering.\n\n";
        return 0;
    }
    const std::string& diff = filtered.patch_text;

    const ai::PromptBuilder prompt_builder;

    const std::string db_path = (config::get_config_dir() / "mygit.db").string();

    // Constructed early so its background worker thread is up and ready by
    // the time inference finishes (bottleneck #2 — async DB writes). The
    // write itself overlaps with commit-message generation and the git
    // commit below rather than blocking the CLI right after the review.
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
    }

    ui::print_report(result);

    const decision_engine::DecisionEngine engine;
    const bool allowed = engine.should_allow(result, /*force_ai=*/false);
    ui::print_verdict(allowed, false);

    logger::log_review(result, allowed, "commit", inference_ms);

    // Persist to SQLite memory system without blocking the CLI's exit.
    const git::GitStatus status;
    db_writer.save_review_async(result, status.get_current_branch(), !allowed);

    if (!allowed) return 1;

    // --- Commit message handling -------------------------------------------

    // If the user supplied -m, use it directly.
    if (!message.empty()) {
        return git::run_git("commit -m \"" + message + "\"");
    }

    // No -m flag: generate a commit message from the diff. Reuses the model
    // loaded during review on a cache miss; loads it fresh here if review was
    // entirely served from cache and never loaded it.
    std::string generated_message;
    {
        ui::Spinner spinner("Generating commit message...");
        try {
            const std::string commit_prompt = prompt_builder.build_commit_message_prompt(diff);
            generated_message =
                get_or_create_llama(llama_owner, cfg).generate_commit_message(commit_prompt);
        } catch (const std::exception& e) {
            spinner.stop();
            std::cerr << "\n  Commit message generation failed: " << e.what() << "\n\n";
            std::cout << "  Falling back to git commit (editor).\n\n";
            return git::run_git("commit");
        }
    }

    if (generated_message.empty()) {
        std::cout << "\n  Could not generate a commit message. Opening editor.\n\n";
        return git::run_git("commit");
    }

    // Show the suggested message.
    std::cout << "\n  Suggested commit message:\n";
    std::cout << "  \033[1;36m" << generated_message << "\033[0m\n";

    const char choice = prompt_user_choice();

    if (choice == 'y') {
        // Use the generated message as-is.
        return git::run_git("commit -m \"" + generated_message + "\"");
    }

    if (choice == 'e') {
        // Open editor with the message pre-filled.
        const std::string edited = open_editor_with(generated_message);
        if (edited.empty()) {
            std::cout << "\n  Empty message — aborting commit.\n\n";
            return 1;
        }
        return git::run_git("commit -m \"" + edited + "\"");
    }

    // 'n' or anything else: open git's default editor.
    return git::run_git("commit");
}

}  // namespace mygit::commands
