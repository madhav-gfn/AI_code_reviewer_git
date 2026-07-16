#pragma once

#include <string>

namespace mygit::ai {

// A prompt split into a static, cacheable prefix (system instructions, JSON
// schema, grammar guidance) and a dynamic suffix (the diff itself). Lets
// LlamaClient cache the prefix's KV state across calls that share it (Level 1
// prefix KV caching) instead of re-evaluating it every time.
struct SplitPrompt {
    std::string prefix;
    std::string suffix;
};

// Builds prompts for the local model (FR-3). Kept separate from LlamaClient
// so prompt iteration doesn't touch the inference code.
class PromptBuilder {
public:
    // Builds a review prompt for the given diff, instructing the model to
    // respond using the FR-5 JSON schema.
    std::string build_review_prompt(const std::string& diff) const;

    // Builds a prompt that instructs the model to generate a single
    // Conventional Commits message from the given diff.
    std::string build_commit_message_prompt(const std::string& diff) const;

    // Builds a focused review prompt scoped to a single file's diff (Level 1
    // batched per-file processing). Uses the same FR-5 JSON schema as
    // build_review_prompt() so results from both can be parsed identically.
    std::string build_single_file_review_prompt(const std::string& file_path,
                                                 const std::string& file_diff) const;

    // Split equivalents of the above. The prefix is identical across calls
    // for a given prompt kind (build_single_file_review_prompt_split's
    // prefix does NOT depend on file_path, so it stays cacheable across an
    // entire per-file review batch).
    SplitPrompt build_review_prompt_split(const std::string& diff) const;
    SplitPrompt build_commit_message_prompt_split(const std::string& diff) const;
    SplitPrompt build_single_file_review_prompt_split(const std::string& file_path,
                                                       const std::string& file_diff) const;
};

}  // namespace mygit::ai
