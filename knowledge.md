# mygit — Project Knowledge Base

## What This Is

`mygit` is a C++20 CLI tool that wraps Git with local AI-powered code review.
Before any commit or push, it diffs staged changes, sends them to a local LLM
(via llama.cpp), and blocks the operation if critical issues are found.
No internet. No API keys. Everything runs on the developer's machine.

---

## Current Status (what actually works)

| Feature | Status |
|---|---|
| `mygit setup` | Saves model path to `~/.mygit/config.json` |
| `mygit review` | Diffs staged changes, runs real LLM inference, prints colored report |
| `mygit commit -m "msg"` | Reviews then runs `git commit` if clean |
| `mygit push origin main` | Reviews then runs `git push` if clean |
| `--force-ai` flag | Overrides critical block on push |
| GBNF grammar-constrained output | JSON is structurally guaranteed valid |
| Colored terminal output | ANSI colors, Windows VT processing enabled |
| Spinner | Rotating animation while model loads + infers |
| Config system | `~/.mygit/config.json` with model_path + gpu_layers |
| Decision engine | Blocks on Critical, passes Medium/High/Low |
| Catch2 unit tests | 4 tests for decision engine, all passing |

---

## Architecture

```
AI_code_reviewer_git/
├── src/
│   ├── main.cpp              # CLI entry point, routes commands
│   └── types.cpp             # Severity <-> string helpers
├── include/mygit/
│   └── types.h               # ReviewResult, Issue, Severity — shared across all modules
├── config/
│   ├── mygit_config.h
│   └── mygit_config.cpp      # Reads/writes ~/.mygit/config.json via nlohmann/json
├── commands/
│   ├── setup_command.cpp     # Interactive first-time wizard
│   ├── review_command.cpp    # Standalone review of staged changes
│   ├── push_command.cpp      # Review then git push
│   └── commit_command.cpp    # Review then git commit
├── git/
│   ├── git_diff.cpp          # `git diff --staged` via popen/shell (V1; libgit2 is V2)
│   ├── git_status.cpp        # Branch name, has_staged_changes
│   └── git_runner.cpp        # `std::system("git <args>")` — executes real git
├── ai/
│   ├── llama_client.cpp      # llama.cpp model load + inference + GBNF grammar sampler
│   └── prompt_builder.cpp    # Builds the review prompt string
├── parsers/
│   └── json_parser.cpp       # Parses model JSON output into ReviewResult
├── decision_engine/
│   └── decision_engine.cpp   # Blocks on Critical; --force-ai overrides
├── reports/
│   └── report_generator.cpp  # Plain-text fallback reporter (largely replaced by ui/)
├── ui/
│   ├── terminal_ui.h
│   └── terminal_ui.cpp       # Spinner, ANSI colors, print_report, print_verdict
├── database/
│   └── sqlite_manager.cpp    # Opens ~/.mygit/mygit.db — stub, no schema yet (V3)
├── tests/
│   └── test_decision_engine.cpp
├── CMakeLists.txt
├── CMakePresets.json          # Ninja + MSVC via strategy:external for Windows
└── vcpkg.json                 # nlohmann-json, fmt, spdlog, sqlite3, ftxui, catch2, llama-cpp
```

---

## Key Technical Decisions

### GBNF Grammar-Constrained Output
The model's output is constrained to the exact JSON schema at the sampler level.
It is structurally impossible for llama.cpp to produce malformed JSON.
Grammar is defined in `ai/llama_client.cpp` as `kReviewGrammar`.
This is the main reliability lever — not prompt engineering.

### Config at `~/.mygit/config.json`
```json
{
  "model_path": "C:/Users/madmi/.mygit/models/model.gguf",
  "gpu_layers": 0
}
```
Every command reads this at startup. If it doesn't exist, the user is told
to run `mygit setup`. Model path is no longer relative to cwd.

### Model
**Qwen2.5-Coder-1.5B-Instruct-GGUF** at Q4_K_M quantization (~1.12 GB).
Download: `https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf`
Better quality options (slower): 3B or 7B variant, same URL pattern.

### Chat Template
`llama_chat_apply_template()` is called automatically using the template
embedded in the GGUF file. No hardcoded prompt format — works across model families.

### Windows-Specific
- `CMakePresets.json` uses `"strategy": "external"` so VS sets up the full MSVC
  developer environment before CMake runs — avoids cl.exe/link.exe version mismatch.
- `terminal_ui.cpp` calls `SetConsoleMode(...ENABLE_VIRTUAL_TERMINAL_PROCESSING)`
  and `SetConsoleOutputCP(CP_UTF8)` at startup for ANSI color support.
- Git subprocess uses `_popen`/`_pclose` on Win32.

### Build System
```
cmake --preset default    # Debug, vcpkg installs deps on first run (~15-20 min)
cmake --preset release    # Release
ctest --preset default    # Run tests
```

---

## Dependencies (vcpkg.json)

| Package | Used for |
|---|---|
| `llama-cpp` | Local LLM inference (llama.cpp + ggml) |
| `nlohmann-json` | Config read/write, JSON parsing of model output |
| `sqlite3` | Future memory system (V3) — connection opened, no schema yet |
| `ftxui` | In vcpkg.json, not yet used in code — reserved for richer TUI (next) |
| `fmt` | String formatting (available, not heavily used yet) |
| `spdlog` | Logging (available, not wired in yet) |
| `catch2` | Unit tests |

---

## JSON Schema (FR-5)

Model output is constrained to:
```json
{
  "safe": false,
  "issues": [
    {
      "severity": "critical",
      "file": "parser.cpp",
      "line": 53,
      "message": "Possible nullptr dereference"
    }
  ]
}
```
Severity levels: `critical`, `high`, `medium`, `low`.
Only `critical` blocks the operation. Others are reported but pass through.

---

## What Is NOT Done Yet

| Feature | Where it lives | Notes |
|---|---|---|
| `mygit install` / PATH setup | — | User manually adds build dir to PATH |
| FTXUI richer UI | `ui/` | In deps, not used yet |
| spdlog review logging | `logs/` | Wired in, not called |
| SQLite memory | `database/` | Connection opens, no schema |
| libgit2 | `git/` | V2 — currently shell out to git |
| RAG system | `rag/` | V4 — FAISS + doc indexing |
| Multi-agent | `agents/` | V5 — specialist agents |
| GPU inference | `ai/` | `MYGIT_GPU_LAYERS` env var wired, untested |
| PR summary | — | Future |
| Commit message generation | `commands/commit_command.cpp` | FR-8, not done |

---


---
