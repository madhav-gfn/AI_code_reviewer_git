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

std::string PromptBuilder::build_commit_message_prompt(const std::string& diff) const {
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
        "- Use imperative mood (\"add\" not \"added\")\n\n"
        "Diff:\n" + diff;
}

}  // namespace mygit::ai
