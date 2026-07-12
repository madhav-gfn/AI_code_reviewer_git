#pragma once

#include <string>
#include "mygit/types.h"

namespace mygit::logger {

void log_review(const ReviewResult& result, bool allowed, const std::string& command, long long inference_ms);

}  // namespace mygit::logger
