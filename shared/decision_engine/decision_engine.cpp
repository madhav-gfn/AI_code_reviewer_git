#include "decision_engine/decision_engine.h"

#include <algorithm>

namespace mygit::decision_engine {

bool DecisionEngine::should_allow(const ReviewResult& result, bool force_ai) const {
    if (force_ai) {
        return true;
    }

    const bool has_critical = std::any_of(
        result.issues.begin(), result.issues.end(),
        [](const Issue& issue) { return issue.severity == Severity::Critical; });

    return !has_critical;
}

}  // namespace mygit::decision_engine
