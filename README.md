# mygit — AI Code Reviewer Git

C++20 wrapper around Git that runs a local LLM review before letting you push.

## Status: V1 scaffold

What's wired up:
- CLI (`push`, `commit`, `review`) — FR-1
- Staged diff extraction via shell `git` calls — FR-2
- JSON parsing of review output — FR-5
- Decision engine (block on Critical, `--force-ai` override) — FR-4
- Console report output — FR-6

What's stubbed and needs the next pass:
- `ai/llama_client.cpp` returns a fixed `{"safe": true, "issues": []}` —
  swap in a real llama.cpp call (model loading, inference) once a model is
  picked and dropped into `models/`.
- `commit` command is a no-op (FR-8 commit message generation, FR-9 hooks).
- The actual `git push`/`git commit` invocation after a passing review.
- `database/sqlite_manager` only opens a connection; no schema yet (Memory
  System is a V3/future requirement).

## Prerequisites

- A C++20 compiler (GCC 13+ / Clang 17+ / MSVC 19.3+)
- CMake 3.21+
- Ninja
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set in your
  environment

## Build

```bash
git clone <this repo>
cd AI_code_reviewer_git

# vcpkg will install dependencies declared in vcpkg.json on first configure
cmake --preset default
cmake --build --build-preset default
```

Release build:

```bash
cmake --preset release
cmake --build --build-preset release
```

## Run

```bash
./build/default/mygit review
./build/default/mygit push origin main
./build/default/mygit push origin main --force-ai
```

## Test

```bash
ctest --preset default
```

## Project layout

Matches the structure in the spec: `commands/` (CLI command handlers),
`git/` (shell-based git access, FR-2), `ai/` (prompt building + model
client, FR-3), `parsers/` (FR-5 JSON parsing), `decision_engine/` (FR-4),
`reports/` (FR-6), `database/` (future memory system), `rag/` and `agents/`
(empty, reserved for V4/V5).
