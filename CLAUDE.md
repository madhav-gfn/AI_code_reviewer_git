# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`mygit` is a C++20 CLI that wraps Git with a local, LLM-powered code review gate. It diffs staged
changes via libgit2, runs a local `.gguf` model through llama.cpp with a GBNF grammar that forces
valid JSON output, and blocks `commit`/`push` when the review finds a critical issue. Everything
(inference, embeddings, storage) runs on the developer's machine — no network calls, no API keys.
See `README.md` for the full feature list and `docs/architecture_review.md` for the roadmap/rationale
behind each subsystem (daemon, async DB, RAG, rename handling, diff limits). `docs/knowledge.md` is
stale relative to the current codebase — don't trust its "what actually works" table without
checking the code.

## Build

Requires a C++20 compiler, CMake 3.21+, Ninja, and a bootstrapped vcpkg (`VCPKG_ROOT` set). Windows-only presets.

```powershell
cmake --preset default
cmake --build build/default
```

Optional: build with GPU-accelerated embeddings (needs cuDNN installed manually and a vcpkg overlay
port — see the "RAG Setup Guide" in README.md before trying this):

```powershell
cmake --preset default -DVCPKG_MANIFEST_FEATURES=onnx-cuda -DMYGIT_ENABLE_ONNX_CUDA=ON
cmake --build build/default --target mygit
```

## Tests

Catch2, built as `mygit_tests` alongside the main binary (`MYGIT_BUILD_TESTS`, on by default).

```powershell
# all tests
build\default\mygit_tests.exe
# or
ctest --preset default

# single test case / tag
build\default\mygit_tests.exe "some test case name"
build\default\mygit_tests.exe "[tag]"
```

Test files live directly under `tests/` and are listed explicitly in `CMakeLists.txt` — a new test
file needs to be added to the `mygit_tests` executable's source list there, it isn't auto-discovered.

## Architecture

**Flow:** `src/main.cpp` parses argv and dispatches to `commands/*` (one file per subcommand:
`review`, `commit`, `push`, `history`, `init`, `setup`, `install`, `daemon`). Commands pull a diff
from `git/` (libgit2-backed), optionally enrich it with retrieved context from `rag/`, build a prompt
via `ai/prompt_builder`, send it to the daemon via `ai/llama_client` + `daemon/daemon_client`, parse
the grammar-guaranteed JSON response with `parsers/json_parser`, evaluate it with
`decision_engine/decision_engine`, persist the verdict via `database/` (async), and render output
with `ui/terminal_ui` (FTXUI).

**Everything links into one static lib, `mygit_core`** (defined in the top-level `CMakeLists.txt`);
`mygit.exe` and `mygit_tests.exe` both just link against it. `mygit_core` is explicitly `STATIC` —
don't drop that, the `x64-windows-release` vcpkg triplet defaults to dynamic linkage, and nothing in
this codebase is `__declspec(dllexport)`-annotated, so a shared build silently exports zero symbols.

**Daemon architecture:** the model is expensive to load, so `daemon/daemon_server` runs it in a
long-lived background HTTP process (`/review`, `/commit`, `/health`, `/shutdown`); `commands/*` talk
to it through `daemon/daemon_client`, which auto-spawns the daemon if it isn't already running and
polls `/health`. The daemon self-shuts-down after 15 minutes idle. If you're changing request/response
shapes, both `daemon_server.cpp` and `daemon_client.h` need to agree, and so does whatever `ai/*`
prompt-building code constructs the payload.

**libgit2 wrapping:** raw `git_*` pointers are never passed around bare — every call site wraps them
immediately in `std::unique_ptr<git_T, void(*)(git_T*)>` bound to the matching `git_T_free` function
(see `git/git_diff.cpp`, `git/git_status.cpp`). Follow that pattern for any new libgit2 call rather
than adding manual free calls.

**RAG is optional by construction, not by config flag.** `rag::RagOrchestrator` (see
`rag/rag_orchestrator.h`) never throws on construction; if the embedding model/tokenizer/index aren't
present, `available()` is `false` and every other method becomes a safe no-op (empty context string,
zero stats). Every caller is expected to invoke it unconditionally and treat "not available" as
identical to "no RAG" rather than branching around it — preserve that contract in any code that
touches RAG. Per-repo RAG storage lives under `.git/mygit/` (`rag.db` + `rag.index`); the (large) ONNX
model weights themselves live globally under `~/.mygit/models/`, shared across repos.

**Multi-language parsing:** Tree-sitter grammars (C++, Python, JS, TS, Go, Rust, Java) are fetched in
`CMakeLists.txt` by downloading each grammar's release tarball directly and compiling `parser.c`/
`scanner.c` into a small static lib — deliberately *not* `FetchContent_MakeAvailable`, because the
grammar repos' own `CMakeLists.txt` try to regenerate `parser.c` via the `tree-sitter` CLI, which
isn't installed here. Adding a new language means adding another `add_tree_sitter_grammar(name,
version)` call (or copying the TypeScript special-case if the grammar repo has multiple
sub-grammars), plus a corresponding AST walker in `rag/code_parser.cpp`. Files without a grammar fall
back to a sliding-window text chunker in the same file, so every file type gets indexed somehow.

**Config paths:** global config/model paths live under `~/.mygit/` (`config.json`, `models/`,
`mygit.db`); per-repo state lives under `.git/mygit/`. `config/mygit_config.*` resolves these.
