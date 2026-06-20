#include "ai/prompt_builder.h"

namespace mygit::ai {

std::string PromptBuilder::build_review_prompt(const std::string& diff) const {
    // TODO: tune this prompt once real model output (FR-3/FR-5) can be
    // observed. Kept minimal and explicit about the required JSON schema.
    return
        "You are a strict code reviewer. Review the following git diff for "
        "bugs, performance issues, and security vulnerabilities.\n\n"
        "Respond with ONLY a JSON object of this exact shape, no prose:\n"
        "{\n"
        "  \"safe\": boolean,\n"
        "  \"issues\": [\n"
        "    {\"severity\": \"critical|high|medium|low\", \"file\": string, "
        "\"line\": number, \"message\": string}\n"
        "  ]\n"
        "}\n\n"
        "Diff:\n" + diff;
}

}  // namespace mygit::ai
