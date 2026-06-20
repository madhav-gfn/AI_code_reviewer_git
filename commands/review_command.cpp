#include "commands/review_command.h"

#include <iostream>

#include "ai/llama_client.h"
#include "ai/prompt_builder.h"
#include "git/git_diff.h"
#include "parsers/json_parser.h"
#include "reports/report_generator.h"

namespace mygit::commands {

int run_review() {
    const git::GitDiff diff_provider;
    const std::string diff = diff_provider.get_staged_diff();

    if (diff.empty()) {
        std::cout << "No staged changes to review.\n";
        return 0;
    }

    const ai::PromptBuilder prompt_builder;
    const std::string prompt = prompt_builder.build_review_prompt(diff);

    const ai::LlamaClient llama_client;
    const std::string raw_response = llama_client.review(prompt);

    const parsers::JsonParser parser;
    const ReviewResult review_result = parser.parse_review(raw_response);

    const reports::ReportGenerator reporter;
    reporter.print(review_result);

    return review_result.safe ? 0 : 1;
}

}  // namespace mygit::commands
