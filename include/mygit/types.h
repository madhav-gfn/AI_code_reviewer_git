#pragma once

#include <string>
#include <vector>

namespace mygit {

// Mirrors the "severity" values in the FR-5 JSON schema.
enum class Severity { Low, Medium, High, Critical };

std::string to_string(Severity severity);
Severity severity_from_string(const std::string& value);

struct Issue {
    Severity severity = Severity::Low;
    std::string file;
    int line = 0;
    std::string message;
};

struct ReviewResult {
    bool safe = true;
    std::vector<Issue> issues;
};

}  // namespace mygit
