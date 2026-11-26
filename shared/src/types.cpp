#include "mygit/types.h"

#include <algorithm>
#include <cctype>

namespace mygit {

std::string to_string(Severity severity) {
    switch (severity) {
        case Severity::Low: return "low";
        case Severity::Medium: return "medium";
        case Severity::High: return "high";
        case Severity::Critical: return "critical";
    }
    return "low";
}

Severity severity_from_string(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                    [](unsigned char c) { return std::tolower(c); });

    if (lowered == "critical") return Severity::Critical;
    if (lowered == "high") return Severity::High;
    if (lowered == "medium") return Severity::Medium;
    return Severity::Low;
}

}  // namespace mygit
