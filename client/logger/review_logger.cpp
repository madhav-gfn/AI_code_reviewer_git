#include "logger/review_logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>
#include <memory>
#include <mutex>

#include "config/mygit_config.h"

namespace mygit::logger {

namespace {

std::shared_ptr<spdlog::logger> get_logger() {
    static std::shared_ptr<spdlog::logger> logger;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);
    if (!logger) {
        auto log_path = config::get_config_dir() / "logs" / "mygit.log";
        
        // Ensure the directory exists
        std::error_code ec;
        std::filesystem::create_directories(log_path.parent_path(), ec);

        // 5MB max size, 3 rotated files
        auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_path.string(), 1024 * 1024 * 5, 3);
        
        logger = std::make_shared<spdlog::logger>("mygit_logger", rotating_sink);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        logger->flush_on(spdlog::level::info);
        
        spdlog::register_logger(logger);
    }
    return logger;
}

} // namespace

void log_review(const ReviewResult& result, bool allowed, const std::string& command, long long inference_ms) {
    int low = 0, medium = 0, high = 0, critical = 0;
    for (const auto& issue : result.issues) {
        switch (issue.severity) {
            case Severity::Low: low++; break;
            case Severity::Medium: medium++; break;
            case Severity::High: high++; break;
            case Severity::Critical: critical++; break;
        }
    }

    auto logger = get_logger();
    logger->info("cmd={} allowed={} inference_ms={} issues_total={} low={} med={} high={} crit={}",
                 command, allowed ? "true" : "false", inference_ms,
                 result.issues.size(), low, medium, high, critical);
}

}  // namespace mygit::logger
