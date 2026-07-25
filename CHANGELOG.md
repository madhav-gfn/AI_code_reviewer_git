# Changelog

All notable changes to the `mygit` project are documented in this file.

---

## [Unreleased] - 2026-07-23

### 🐛 Bug Fixes & Architecture Improvements

#### 1. Repository RAG Storage Isolation
- **Reason**: Previously, `rag.db` and `rag.index` were stored globally in `~/.mygit/`. Because files were indexed using relative paths without a repository namespace, running `mygit commit` in one repository would overwrite and tombstone vector index entries from all other repositories.
- **Fix**: Updated `commands/commit_command.cpp` to resolve the current Git repository's `.git/` directory dynamically.
- **Result**:
  - Vector index (`.git/mygit/rag.index`) and RAG metadata DB (`.git/mygit/rag.db`) are now strictly isolated per-repository inside `.git/mygit/`.
  - Embeddings from different repositories will never clash or overwrite each other.
  - The heavy ONNX neural network weights (`embedding_model.onnx` and `tokenizer.json`) remain stored globally at `~/.mygit/models/` to save disk space.

#### 2. RAG Global Path Resolution
- **Reason**: `RagOrchestrator` loaded tokenizer and ONNX embedding models using relative paths (`models/tokenizer.json`), which caused model loading failures when running `mygit commit` outside the project root.
- **Fix**: Standardized model path resolution to always target `~/.mygit/models/` via `config::get_config_dir()`.

#### 3. BPE Tokenizer Crash Guard
- **Reason**: HuggingFace `tokenizer.json` files sometimes specify `"unk_token": null`. Attempting to extract `null` as a `std::string` threw an uncaught `nlohmann::json` type exception.
- **Fix**: Added type check in `rag/bpe_tokenizer.cpp` before calling `.get<std::string>()`.

#### 4. ONNX Runtime Warning Suppression
- **Reason**: Static linking of ONNX Runtime on Windows resulted in hundreds of harmless `Schema error` registration warnings printed to stdout.
- **Fix**: Raised `Ort::Env` logging level to `ORT_LOGGING_LEVEL_ERROR` in `rag/embedder.cpp`.

---

### ✨ Features & Enhancements

#### 1. Repository Name Tracking in History
- **Reason**: When running `mygit history`, users could not see which repository a review record belonged to.
- **What Changed**:
  - Updated `database::SqliteManager` schema to add `repo_name` column to `reviews` table.
  - Added automatic database migration logic to update existing SQLite databases seamlessly.
  - Modified `commands/history_command.cpp` to include a new **Repo** column in the FTXUI interactive terminal table.
  - Updated `database::AsyncDbWriter` to capture and record repository names during review logging.
