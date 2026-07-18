#include "rag/rag_orchestrator.h"

#include <atomic>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#include "database/sqlite_manager.h"  // hash_diff_content: reused as a generic content fingerprint
#include "git/git_runner.h"
#include "rag/code_parser.h"
#include "rag/embedder.h"
#include "rag/vector_store.h"

namespace mygit::rag {

namespace {

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

// Embeds `units` across a small bounded worker pool (Embedder::embed() is
// documented safe for concurrent calls on the same instance), rather than
// one at a time - this is the ingestion-side parallelism the architecture
// review's Level 1 "batched processing" pattern already applies elsewhere
// in the pipeline (see commands/commit_command.cpp's per-file review
// batching).
std::vector<std::optional<std::vector<float>>> embed_units_parallel(
    const Embedder& embedder, const std::vector<CodeUnit>& units) {
    std::vector<std::optional<std::vector<float>>> results(units.size());
    if (units.empty()) return results;

    const unsigned hw = std::thread::hardware_concurrency();
    const size_t worker_count = std::max<size_t>(1, std::min<size_t>(units.size(), hw ? hw : 4));

    std::atomic<size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&]() {
            for (size_t i = next_index.fetch_add(1); i < units.size(); i = next_index.fetch_add(1)) {
                results[i] = embedder.embed(units[i].content);
            }
        });
    }
    for (std::thread& t : workers) t.join();
    return results;
}

}  // namespace

struct RagOrchestrator::Impl {
    CodeParser parser;
    Embedder embedder;
    VectorStore store;

    Impl(std::string model_path, std::string tokenizer_path, std::string index_path,
         std::string db_path)
        : embedder(std::move(model_path), std::move(tokenizer_path)),
          store(std::move(index_path), std::move(db_path)) {}
};

RagOrchestrator::RagOrchestrator(std::string db_path, std::string index_path,
                                  std::string model_path, std::string tokenizer_path)
    : impl_(std::make_unique<Impl>(std::move(model_path), std::move(tokenizer_path),
                                    std::move(index_path), std::move(db_path))) {}

RagOrchestrator::~RagOrchestrator() = default;

bool RagOrchestrator::available() const {
    return impl_ && impl_->embedder.available() && impl_->store.available();
}

void RagOrchestrator::update_index() {
    if (!available()) return;

    try {
        const std::vector<std::string> tracked = split_lines(git::run_git_capture("ls-files"));
        std::unordered_set<std::string> tracked_set;

        for (const std::string& file_path : tracked) {
            if (!is_cpp_source_file(file_path)) continue;
            tracked_set.insert(file_path);

            const std::optional<std::string> content = read_file(file_path);
            if (!content) continue;  // unreadable (deleted between ls-files and now, permissions, ...)

            const std::string hash = database::hash_diff_content(file_path, *content);
            if (hash == impl_->store.get_indexed_hash(file_path)) continue;  // unchanged, skip

            impl_->store.remove_file(file_path);  // safe no-op if never indexed before

            const std::vector<CodeUnit> units = impl_->parser.parse_source(file_path, *content);
            if (!units.empty()) {
                const std::vector<std::optional<std::vector<float>>> embedded =
                    embed_units_parallel(impl_->embedder, units);

                std::vector<CodeUnit> ok_units;
                std::vector<std::vector<float>> ok_embeddings;
                ok_units.reserve(units.size());
                ok_embeddings.reserve(units.size());
                for (size_t i = 0; i < units.size(); ++i) {
                    if (embedded[i]) {
                        ok_units.push_back(units[i]);
                        ok_embeddings.push_back(*embedded[i]);
                    }
                }
                impl_->store.add_units(ok_units, ok_embeddings);
            }

            impl_->store.set_indexed_hash(file_path, hash);
        }

        // Purge files that were indexed previously but are no longer
        // tracked (deleted, or renamed - git ls-files reports renames as a
        // delete-at-old-path + add-at-new-path pair, so the new path is
        // handled by the loop above and the old one is caught here).
        for (const std::string& previously_indexed : impl_->store.indexed_file_paths()) {
            if (!tracked_set.count(previously_indexed)) {
                impl_->store.remove_file(previously_indexed);
            }
        }

        impl_->store.save();
    } catch (const std::exception& e) {
        std::cerr << "[rag] index update failed: " << e.what() << "\n";
    }
}

std::string RagOrchestrator::get_context_for_diff(const std::string& patch_text, int top_k) const {
    if (!available() || patch_text.empty()) return "";

    try {
        const std::optional<std::vector<float>> query = impl_->embedder.embed(patch_text);
        if (!query) return "";

        const std::vector<CodeUnit> hits = impl_->store.search(*query, top_k);
        if (hits.empty()) return "";

        std::ostringstream out;
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i > 0) out << "\n\n";
            out << "--- Context from " << hits[i].file_path << " ---\n" << hits[i].content;
        }
        return out.str();
    } catch (const std::exception& e) {
        std::cerr << "[rag] context retrieval failed: " << e.what() << "\n";
        return "";
    }
}

}  // namespace mygit::rag
