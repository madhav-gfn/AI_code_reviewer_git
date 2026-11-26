#pragma once

#include <string>

#include "mygit/types.h"

namespace mygit::parsers {

// Parses the model's raw JSON response into a ReviewResult (FR-5).
class JsonParser {
public:
    // Throws std::runtime_error if the response is not valid JSON matching
    // the expected schema.
    ReviewResult parse_review(const std::string& raw_response) const;
};

}  // namespace mygit::parsers
