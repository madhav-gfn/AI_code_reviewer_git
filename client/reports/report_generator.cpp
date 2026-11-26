#include "reports/report_generator.h"

#include <iostream>

namespace mygit::reports {

void ReportGenerator::print(const ReviewResult& result) const {
    if (result.issues.empty()) {
        std::cout << "No issues found.\n";
        return;
    }

    for (const auto& issue : result.issues) {
        std::cout << "\n"
                   << to_string(issue.severity) << " Issue\n"
                   << issue.file << ":" << issue.line << "\n"
                   << issue.message << "\n";
    }
    std::cout << "\n";
}

}  // namespace mygit::reports
