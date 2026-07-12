#include "commands/review_command.h"

#include <exception>
#include <iostream>
#include <chrono>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "config/mygit_config.h"
#include "decision_engine/decision_engine.h"
#include "git/git_diff.h"
#include "logger/review_logger.h"
#include "parsers/json_parser.h"
#include "ui/terminal_ui.h"

namespace mygit::commands {

int run_review() {
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
        std::cout << "\n  No staged changes to review.\n\n";
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
    }  // spinner stops and clears here

    const parsers::JsonParser parser;
    const ReviewResult result = parser.parse_review(raw_response);
    ui::print_report(result);

    logger::log_review(result, result.safe, "review", inference_ms);

    return result.safe ? 0 : 1;
}

}  // namespace mygit::commands
