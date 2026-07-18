#include "ai/prompt_builder.h"

namespace mygit::ai {

namespace {

std::string review_instructions() {
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
        "}\n\n";
}

std::string commit_message_instructions() {
    return
        "You are a commit message generator. Given the following git diff, "
        "write a single-line commit message following the Conventional Commits format:\n\n"
        "  <type>(<scope>): <short description>\n\n"
        "Types: feat, fix, docs, style, refactor, perf, test, build, ci, chore\n"
        "Scope: the module, file, or component most affected (keep it short)\n\n"
        "Rules:\n"
        "- Return ONLY the commit message, nothing else\n"
        "- No quotes, no explanation, no markdown\n"
        "- Keep it under 72 characters\n"
        "- Use imperative mood (\"add\" not \"added\")\n\n";
}

}  // namespace

std::string PromptBuilder::build_review_prompt(const std::string& diff) const {
    return review_instructions() + "Diff:\n" + diff;
}

std::string PromptBuilder::build_commit_message_prompt(const std::string& diff) const {
    return build_commit_message_prompt(diff, "");
}

std::string PromptBuilder::build_commit_message_prompt(const std::string& diff,
                                                         const std::string& context) const {
    if (context.empty()) {
        return commit_message_instructions() + "Diff:\n" + diff;
    }
    return commit_message_instructions() +
           "Repository context (for reference; do not describe it as changed):\n" + context +
           "\n\nDiff:\n" + diff;
}

std::string PromptBuilder::build_single_file_review_prompt(const std::string& file_path,
                                                             const std::string& file_diff) const {
    return review_instructions() + "File: " + file_path + "\nDiff:\n" + file_diff;
}

SplitPrompt PromptBuilder::build_review_prompt_split(const std::string& diff) const {
    return SplitPrompt{review_instructions() + "Diff:\n", diff};
}

SplitPrompt PromptBuilder::build_commit_message_prompt_split(const std::string& diff) const {
    return SplitPrompt{commit_message_instructions() + "Diff:\n", diff};
}

SplitPrompt PromptBuilder::build_commit_message_prompt_split(const std::string& diff,
                                                               const std::string& context) const {
    if (context.empty()) {
        return build_commit_message_prompt_split(diff);
    }
    // Prefix stays exactly the constant instruction block (cacheable);
    // context + diff both live in the suffix since both vary per call.
    return SplitPrompt{commit_message_instructions(),
                        "Repository context (for reference; do not describe it as changed):\n" +
                            context + "\n\nDiff:\n" + diff};
}

SplitPrompt PromptBuilder::build_single_file_review_prompt_split(
    const std::string& file_path, const std::string& file_diff) const {
    return SplitPrompt{review_instructions(), "File: " + file_path + "\nDiff:\n" + file_diff};
}

}  // namespace mygit::ai
