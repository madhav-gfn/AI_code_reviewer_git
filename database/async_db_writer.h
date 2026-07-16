#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "mygit/types.h"

namespace mygit::database {

// Fire-and-forget async database writer (bottleneck #2). Enqueues write
// operations onto a background thread so the CLI can exit immediately after
// inference. The destructor joins the thread and flushes pending writes.
class AsyncDbWriter {
public:
    explicit AsyncDbWriter(std::string db_path);
    ~AsyncDbWriter();  // joins the worker thread, flushing pending writes

    AsyncDbWriter(const AsyncDbWriter&) = delete;
    AsyncDbWriter& operator=(const AsyncDbWriter&) = delete;

    // Non-blocking. Enqueues the review for background persistence.
    void save_review_async(ReviewResult result, std::string branch, bool blocked);

private:
    struct WorkItem {
        ReviewResult result;
        std::string branch;
        bool blocked;
    };

    void worker_loop();

    std::string db_path_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<WorkItem> queue_;
    bool shutting_down_ = false;
};

}  // namespace mygit::database
