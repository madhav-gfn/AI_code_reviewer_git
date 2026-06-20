#pragma once

#include "mygit/types.h"

namespace mygit::decision_engine {

// Implements FR-4: decides whether an operation may proceed.
class DecisionEngine {
public:
    // Allows the operation unless a Critical issue is present, in which
    // case it blocks - unless force_ai overrides the block (--force-ai).
    bool should_allow(const ReviewResult& result, bool force_ai) const;
};

}  // namespace mygit::decision_engine
