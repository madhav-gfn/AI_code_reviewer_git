#include "ai/review_aggregator.h"

namespace mygit::ai {

ReviewResult aggregate_reviews(const std::vector<ReviewResult>& per_file_results) {
    ReviewResult combined;
    combined.safe = true;
    for (const ReviewResult& r : per_file_results) {
        combined.safe = combined.safe && r.safe;
        combined.issues.insert(combined.issues.end(), r.issues.begin(), r.issues.end());
    }
    return combined;
}

}  // namespace mygit::ai
