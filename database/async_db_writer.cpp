#include "database/async_db_writer.h"

#include "database/sqlite_manager.h"

namespace mygit::database {

AsyncDbWriter::AsyncDbWriter(std::string db_path)
    : db_path_(std::move(db_path)), worker_(&AsyncDbWriter::worker_loop, this) {}

AsyncDbWriter::~AsyncDbWriter() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AsyncDbWriter::save_review_async(ReviewResult result, std::string branch, bool blocked) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(WorkItem{std::move(result), std::move(branch), blocked});
    }
    cv_.notify_one();
}

void AsyncDbWriter::worker_loop() {
    // Opened on the worker thread so SqliteManager's handle is only ever
    // touched from here.
    SqliteManager db(db_path_);

    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });

        while (!queue_.empty()) {
            WorkItem item = std::move(queue_.front());
            queue_.pop();
            lock.unlock();

            if (db.is_open()) {
                db.save_review(item.result, item.branch, item.blocked);
            }

            lock.lock();
        }

        if (shutting_down_) {
            break;
        }
    }
}

}  // namespace mygit::database
