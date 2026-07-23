#include "commands/commit_command.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include "ai/prompt_builder.h"
#include "ai/review_aggregator.h"
#include "config/mygit_config.h"
#include "daemon/daemon_client.h"
#include "database/async_db_writer.h"
#include "database/sqlite_manager.h"
#include "decision_engine/decision_engine.h"
#include "diff_filter/diff_filter.h"
#include "git/git_diff.h"
#include "git/git_runner.h"
#include "git/git_status.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "rag/rag_orchestrator.h"
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

// Makes sure the daemon is reachable (auto-starting it on the first actual
// cache miss) and returns it. Cheap to call afterward - the daemon is only
// probed/spawned once per process, so a fully cached diff (all cache hits)
// never touches the daemon at all, same guarantee the pre-daemon code gave
// for loading the model.
const daemon::DaemonClient& ensure_daemon(const daemon::DaemonClient& client, bool& ready) {
    if (!ready) {
        if (!client.ensure_running()) {
            throw std::runtime_error("Daemon is unreachable and failed to auto-start.");
        }
        ready = true;
    }
    return client;
}

// Reviews `filtered.kept_files`. Each file's diff is fingerprinted
// (database::hash_diff_content) and looked up in `cache` first, so
// re-reviewing a diff that's byte-for-byte unchanged since last time reuses
// the stored result instead of re-running inference. The daemon is only
// contacted (and auto-started if needed) on the first actual cache miss, so
// a fully cached diff never pays the daemon round-trip at all.
//
// Uses one file-per-call batch when there's more than one file (Level 1
// batched per-file processing), a single direct call for exactly one file, or
// the old concatenated-diff prompt once the file count exceeds
// kMaxBatchedFiles. The batch path sends split prompts so the daemon's
// LlamaClient only evaluates the shared system-prompt prefix once and reuses
// its KV state across the whole batch (Level 1 prefix KV caching) - and,
// since the daemon outlives this process, across future review/commit runs
// too.
ReviewResult run_batched_review(const daemon::DaemonClient& daemon_client, bool& daemon_ready,
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
            parser.parse_review(ensure_daemon(daemon_client, daemon_ready).review(prompt));
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
            parser.parse_review(ensure_daemon(daemon_client, daemon_ready).review(prompt));
        cache.save_cached_file_review(hash, result);
        return result;
    }

    // Partition into cache hits and misses before touching the daemon, so a
    // diff that's entirely unchanged since the last review never contacts it.
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
        // Start (or confirm) the daemon once, synchronously, before fanning
        // out - ensure_daemon() mutates daemon_ready and isn't safe to call
        // from multiple threads concurrently.
        ensure_daemon(daemon_client, daemon_ready);

        // Dispatch the misses across a small worker pool instead of one
        // review-then-wait-then-review round trip at a time. DaemonClient
        // opens its own connection per call, so this is safe to do
        // concurrently; the daemon itself still serializes actual model
        // inference behind a mutex (LlamaClient holds a single context), so
        // the win here is overlapping request/response and JSON-parsing
        // overhead across files rather than overlapping inference itself.
        std::vector<ReviewResult> miss_results(misses.size());
        std::exception_ptr first_error;
        std::mutex error_mutex;

        const unsigned hw = std::thread::hardware_concurrency();
        const size_t worker_count = std::max<size_t>(1, std::min<size_t>(misses.size(), hw ? hw : 4));

        std::atomic<size_t> next_index{0};
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (size_t w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                for (size_t i = next_index.fetch_add(1); i < misses.size();
                     i = next_index.fetch_add(1)) {
                    try {
                        const ai::SplitPrompt prompt = prompt_builder.build_single_file_review_prompt_split(
                            display_path(*misses[i].first), misses[i].first->patch);
                        miss_results[i] = parser.parse_review(daemon_client.review(prompt));
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (!first_error) first_error = std::current_exception();
                    }
                }
            });
        }
        for (std::thread& t : workers) t.join();

        if (first_error) std::rethrow_exception(first_error);

        // Cache writes happen sequentially back on this thread rather than
        // from the workers above - SqliteManager wraps a single sqlite3*
        // connection and there's no need to fight over it when the writes
        // are this cheap.
        for (size_t i = 0; i < misses.size(); ++i) {
            cache.save_cached_file_review(misses[i].second, miss_results[i]);
            per_file.push_back(std::move(miss_results[i]));
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
    try {
        config::load_config();  // fail fast with a clear message if not configured
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
    // cache lookups must happen before deciding whether to contact the
    // daemon at all, so they can't go through the async writer.
    database::SqliteManager review_cache(db_path);

    // RAG pipeline (bottleneck #3 — repository-aware context retrieval).
    // Construction never throws; if the embedding model/tokenizer aren't
    // set up, rag_orchestrator.available() is false and everything below
    // becomes a no-op, leaving today's diff-only prompt behavior intact.
    // update_index() is I/O- and embedding-bound and wholly independent of
    // the AI review below (which only talks to the daemon), so it runs on
    // its own thread and overlaps with the review's spinner instead of
    // adding to the CLI's total wall-clock time.
    rag::RagOrchestrator rag_orchestrator(
        db_path, 
        (config::get_config_dir() / "rag.index").string(),
        (config::get_config_dir() / "models" / "embedding_model.onnx").string(),
        (config::get_config_dir() / "models" / "tokenizer.json").string());
    std::thread rag_index_thread;
    if (rag_orchestrator.available()) {
        rag_index_thread = std::thread([&rag_orchestrator]() { rag_orchestrator.update_index(); });
    }
    // run_commit() has several early `return`s below (review failure,
    // blocked review, -m supplied) that all happen before the explicit join
    // near commit-message generation - std::thread's destructor calls
    // std::terminate() on a still-joinable thread, so this guard makes sure
    // every exit path joins it exactly once, however the function returns.
    struct ThreadJoinGuard {
        std::thread& t;
        ~ThreadJoinGuard() { if (t.joinable()) t.join(); }
    } rag_index_join_guard{rag_index_thread};

    ReviewResult result;
    long long inference_ms = 0;
    const daemon::DaemonClient daemon_client;
    bool daemon_ready = false;
    {
        ui::Spinner spinner("Reviewing staged changes...");
        auto start = std::chrono::steady_clock::now();
        try {
            result = run_batched_review(daemon_client, daemon_ready, prompt_builder, filtered,
                                         review_cache);
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

    // Make sure any in-flight index update has finished before asking it for
    // context - it was kicked off in parallel with the review above, so
    // this join is typically a no-op by the time we get here.
    if (rag_index_thread.joinable()) rag_index_thread.join();
    const std::string rag_context = rag_orchestrator.get_context_for_diff(diff);

    // No -m flag: generate a commit message from the diff. Reuses the daemon
    // connection warmed up during review on a cache miss; auto-starts it
    // here if review was entirely served from cache and never touched it.
    std::string generated_message;
    {
        ui::Spinner spinner("Generating commit message...");
        try {
            const std::string commit_prompt =
                prompt_builder.build_commit_message_prompt(diff, rag_context);
            generated_message =
                ensure_daemon(daemon_client, daemon_ready).generate_commit_message(commit_prompt);
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
