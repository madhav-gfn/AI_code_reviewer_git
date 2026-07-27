# Changelog

All notable changes to the `mygit` project are documented in this file.

---

## [Unreleased] - 2026-07-26

### ✨ Features & Enhancements (Architecture Review Remaining Items)

#### 1. Multi-Language Tree-sitter Grammar Support (Bottleneck #3)
- **What Changed**: The RAG code parser now supports **Python, JavaScript, TypeScript, Go, Rust, and Java** in addition to C/C++. Each language has a dedicated AST walker that extracts functions, methods, classes, and structs as `CodeUnit`s.
- **Files**: `CMakeLists.txt` (6 new grammar downloads via tarball-extract macro), `rag/code_parser.h`, `rag/code_parser.cpp`
- **API**: `is_parseable_file()` replaces `is_cpp_source_file()` as the primary check; backward-compatible inline alias retained.

#### 2. Universal Text-Chunking Fallback (Bottleneck #3)
- **What Changed**: Files without a Tree-sitter grammar (Markdown, YAML, config files, etc.) are now indexed using a sliding-window text chunker (~40 lines per chunk, 10-line overlap). Every repository gets embedded regardless of programming language.
- **Files**: `rag/code_parser.cpp` (`text_chunk()`), `rag/rag_orchestrator.cpp` (filter changed to `is_indexable_file()`)

#### 3. `mygit init` Command
- **What Changed**: New CLI command that explicitly builds (or updates) the RAG embedding index for the current Git repository. Index is stored per-repo in `.git/mygit/` (rag.db + rag.index); global ONNX model weights remain in `~/.mygit/models/`.
- **Files**: `commands/init_command.h` [NEW], `commands/init_command.cpp` [NEW], `src/main.cpp` (registered)

#### 4. Repository-Aware Context in Reviews (Bottleneck #3)
- **What Changed**: `mygit review` now retrieves RAG context (same pipeline as `mygit commit`) and includes it in review prompts, so the AI can see what changed functions actually do instead of guessing from diff lines alone. Falls back to zero-RAG behavior if not initialized.
- **Files**: `commands/review_command.cpp`, `ai/prompt_builder.h`, `ai/prompt_builder.cpp` (context-aware review prompt overloads)

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
