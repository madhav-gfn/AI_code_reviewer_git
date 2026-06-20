#pragma once

#include <string>

namespace mygit::ai {

// Builds prompts for the local model (FR-3). Kept separate from LlamaClient
// so prompt iteration doesn't touch the inference code.
class PromptBuilder {
public:
    // Builds a review prompt for the given diff, instructing the model to
    // respond using the FR-5 JSON schema.
    std::string build_review_prompt(const std::string& diff) const;
};

}  // namespace mygit::ai
