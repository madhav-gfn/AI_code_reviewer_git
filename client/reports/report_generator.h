#pragma once

#include "mygit/types.h"

namespace mygit::reports {

// Renders a ReviewResult to stdout (FR-6). A future version may render via
// FTXUI for colored/interactive output.
class ReportGenerator {
public:
    void print(const ReviewResult& result) const;
};

}  // namespace mygit::reports
