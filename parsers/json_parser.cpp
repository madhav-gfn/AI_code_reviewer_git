#include "parsers/json_parser.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace mygit::parsers {

ReviewResult JsonParser::parse_review(const std::string& raw_response) const {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(raw_response);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("Invalid review JSON: ") + e.what());
    }

    ReviewResult result;
    result.safe = parsed.value("safe", true);

    if (parsed.contains("issues") && parsed["issues"].is_array()) {
        for (const auto& item : parsed["issues"]) {
            Issue issue;
            issue.severity = severity_from_string(item.value("severity", "low"));
            issue.file = item.value("file", "");
            issue.line = item.value("line", 0);
            issue.message = item.value("message", "");
            result.issues.push_back(std::move(issue));
        }
    }

    return result;
}

}  // namespace mygit::parsers
