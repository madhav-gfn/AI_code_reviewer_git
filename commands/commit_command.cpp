#include "commands/commit_command.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <chrono>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "config/mygit_config.h"
#include "database/sqlite_manager.h"
#include "decision_engine/decision_engine.h"
#include "git/git_diff.h"
#include "git/git_runner.h"
#include "git/git_status.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "ui/terminal_ui.h"

#include <filesystem>

namespace mygit::commands {

namespace {

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
    const std::string diff = diff_provider.get_staged_diff();
    if (diff.empty()) {
        std::cout << "\n  Nothing staged to commit.\n\n";
        return 0;
    }

    const ai::PromptBuilder prompt_builder;
    const std::string review_prompt = prompt_builder.build_review_prompt(diff);

    std::string raw_response;
    long long inference_ms = 0;
    ai::LlamaClient* llama_ptr = nullptr;
    std::unique_ptr<ai::LlamaClient> llama_owner;
    {
        ui::Spinner spinner("Reviewing staged changes...");
        auto start = std::chrono::steady_clock::now();
        try {
            llama_owner = std::make_unique<ai::LlamaClient>(cfg.model_path, cfg.gpu_layers);
            llama_ptr = llama_owner.get();
            raw_response = llama_ptr->review(review_prompt);
        } catch (const std::exception& e) {
            spinner.stop();
            std::cerr << "\n  AI review failed: " << e.what() << "\n\n";
            return 1;
        }
        auto end = std::chrono::steady_clock::now();
        inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }

    const parsers::JsonParser parser;
    const ReviewResult result = parser.parse_review(raw_response);
    ui::print_report(result);

    const decision_engine::DecisionEngine engine;
    const bool allowed = engine.should_allow(result, /*force_ai=*/false);
    ui::print_verdict(allowed, false);

    logger::log_review(result, allowed, "commit", inference_ms);

    // Persist to SQLite memory system.
    database::SqliteManager db((config::get_config_dir() / "mygit.db").string());
    if (db.is_open()) {
        const git::GitStatus status;
        db.save_review(result, status.get_current_branch(), !allowed);
    }

    if (!allowed) return 1;

    // --- Commit message handling -------------------------------------------

    // If the user supplied -m, use it directly.
    if (!message.empty()) {
        return git::run_git("commit -m \"" + message + "\"");
    }

    // No -m flag: generate a commit message from the diff.
    std::string generated_message;
    {
        ui::Spinner spinner("Generating commit message...");
        try {
            const std::string commit_prompt = prompt_builder.build_commit_message_prompt(diff);
            generated_message = llama_ptr->generate_commit_message(commit_prompt);
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
