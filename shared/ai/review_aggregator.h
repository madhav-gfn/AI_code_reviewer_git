#pragma once

#include <vector>

#include "mygit/types.h"

namespace mygit::ai {

// Merges per-file review results (Level 1 batched per-file processing) into
// a single repository-level ReviewResult: all issues concatenated, safe iff
// every input was safe. Returns a safe, empty result for an empty input.
ReviewResult aggregate_reviews(const std::vector<ReviewResult>& per_file_results);

}  // namespace mygit::ai
