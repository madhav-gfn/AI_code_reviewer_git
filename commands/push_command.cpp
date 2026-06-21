#include "commands/push_command.h"

#include <exception>
#include <iostream>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "decision_engine/decision_engine.h"
#include "git/git_diff.h"
#include "parsers/json_parser.h"
#include "reports/report_generator.h"

namespace mygit::commands {

int run_push(const std::string& remote, const std::string& branch, bool force_ai) {
    const git::GitDiff diff_provider;
    const std::string diff = diff_provider.get_staged_diff();

    if (diff.empty()) {
        std::cout << "No staged changes to review.\n";
        // TODO: invoke the actual `git push <remote> <branch>` here.
        std::cout << "Pushing to " << remote << "/" << branch << " (not yet implemented).\n";
        return 0;
    }

    const ai::PromptBuilder prompt_builder;
    const std::string prompt = prompt_builder.build_review_prompt(diff);

    std::string raw_response;
    try {
        const ai::LlamaClient llama_client;
        raw_response = llama_client.review(prompt);
    } catch (const std::exception& e) {
        std::cerr << "AI review unavailable: " << e.what() << "\n";
        return 1;
    }

    const parsers::JsonParser parser;
    const ReviewResult review_result = parser.parse_review(raw_response);

    const reports::ReportGenerator reporter;
    reporter.print(review_result);

    const decision_engine::DecisionEngine engine;
    if (!engine.should_allow(review_result, force_ai)) {
        std::cerr << "Push blocked: critical issues found. Use --force-ai to override.\n";
        return 1;
    }

    // TODO: invoke the actual `git push <remote> <branch>` here.
    std::cout << "Review passed. Pushing to " << remote << "/" << branch
              << " (not yet implemented).\n";
    return 0;
}

}  // namespace mygit::commands
