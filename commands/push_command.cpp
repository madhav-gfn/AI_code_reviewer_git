#include "commands/push_command.h"

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

int run_push(const std::string& remote, const std::string& branch, bool force_ai) {
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
        std::cout << "\n  No staged changes — pushing directly.\n\n";
        return git::run_git("push " + remote + " " + branch);
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
    const bool allowed = engine.should_allow(result, force_ai);
    ui::print_verdict(allowed, force_ai && !result.safe);

    logger::log_review(result, allowed, "push", inference_ms);

    if (!allowed) return 1;
    return git::run_git("push " + remote + " " + branch);
}

}  // namespace mygit::commands
