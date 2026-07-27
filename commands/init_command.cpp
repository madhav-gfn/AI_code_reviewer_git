#include "commands/init_command.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "config/mygit_config.h"
#include "git/git_runner.h"
#include "rag/rag_orchestrator.h"
#include "ui/terminal_ui.h"

namespace mygit::commands {

int run_init() {
    std::cout << "\n  mygit init\n"
              << "  " << std::string(36, '-') << "\n\n";

    // 1. Make sure we're inside a Git repository.
    const std::string git_dir_raw = git::run_git_capture("rev-parse --git-dir");
    if (git_dir_raw.empty()) {
        std::cerr << "  Error: not inside a Git repository.\n\n";
        return 1;
    }

    const std::string top_level_raw = git::run_git_capture("rev-parse --show-toplevel");
    const std::string repo_name =
        top_level_raw.empty() ? "<unknown>"
                              : std::filesystem::path(top_level_raw).filename().string();

    // 2. Resolve per-repo RAG storage directory (.git/mygit/).
    const std::filesystem::path rag_dir =
        std::filesystem::absolute(git_dir_raw) / "mygit";
    std::error_code ec;
    std::filesystem::create_directories(rag_dir, ec);
    if (ec) {
        std::cerr << "  Error: could not create " << rag_dir << ": " << ec.message() << "\n\n";
        return 1;
    }

    const std::string rag_db_path = (rag_dir / "rag.db").string();
    const std::string rag_index_path = (rag_dir / "rag.index").string();

    // 3. Resolve global embedding model paths.
    std::filesystem::path models_dir;
    try {
        models_dir = config::get_config_dir() / "models";
    } catch (...) {
        // get_config_dir() doesn't throw, but guard anyway.
        models_dir = std::filesystem::path(".mygit") / "models";
    }

    const std::string model_path = (models_dir / "embedding_model.onnx").string();
    const std::string tokenizer_path = (models_dir / "tokenizer.json").string();

    // 4. Create the RAG orchestrator.
    rag::RagOrchestrator rag(rag_db_path, rag_index_path, model_path, tokenizer_path);

    if (!rag.available()) {
        std::cerr << "  Error: RAG pipeline is not available.\n"
                  << "  Make sure the embedding model and tokenizer are installed:\n"
                  << "    " << model_path << "\n"
                  << "    " << tokenizer_path << "\n\n";
        return 1;
    }

    // 5. Run the full index build with a progress spinner.
    std::cout << "  Repository: " << repo_name << "\n"
              << "  Index path: " << rag_dir.string() << "\n\n";

    long long elapsed_ms = 0;
    {
        ui::Spinner spinner("Indexing repository...");
        auto start = std::chrono::steady_clock::now();
        try {
            rag.update_index();
        } catch (const std::exception& e) {
            spinner.stop();
            std::cerr << "\n  Indexing failed: " << e.what() << "\n\n";
            return 1;
        }
        auto end = std::chrono::steady_clock::now();
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }

    // 6. Print stats.
    const auto stats = rag.index_stats();

    std::cout << "\n  \033[1;32m✓ Indexing complete\033[0m\n"
              << "    Files indexed:  " << stats.files_indexed << "\n"
              << "    Time:           " << elapsed_ms << " ms\n"
              << "    Stored at:      " << rag_dir.string() << "\n\n"
              << "  Your reviews and commits will now include repository context.\n\n";

    return 0;
}

}  // namespace mygit::commands
