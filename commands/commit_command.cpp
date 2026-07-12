#include "commands/commit_command.h"

#include <exception>
#include <iostream>
#include <chrono>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "config/mygit_config.h"
#include "decision_engine/decision_engine.h"
#include "git/git_diff.h"
#include "git/git_runner.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "ui/terminal_ui.h"

namespace mygit::commands {

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
    const std::string prompt = prompt_builder.build_review_prompt(diff);

    std::string raw_response;
    long long inference_ms = 0;
    {
        ui::Spinner spinner("Reviewing staged changes...");
        auto start = std::chrono::steady_clock::now();
        try {
            const ai::LlamaClient llama_client(cfg.model_path, cfg.gpu_layers);
            raw_response = llama_client.review(prompt);
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

    if (!allowed) return 1;

    const std::string git_args = message.empty()
        ? "commit"
        : "commit -m \"" + message + "\"";
    return git::run_git(git_args);
}

}  // namespace mygit::commands
