
## Prompts for Next Features

Copy-paste these at the start of a new conversation.

---

### 1. Installable Binary + PATH Setup

```
We are building mygit — a C++20 CLI tool that wraps Git with local LLM-powered
code review (llama.cpp, Qwen2.5-Coder-1.5B-Instruct-GGUF, GBNF grammar-constrained
JSON output). It's built with CMake + vcpkg on Windows (MSVC, Ninja).

Current state: the binary builds to build/default/mygit.exe. The user has to
run it with a full path. The model path is stored in ~/.mygit/config.json via
`mygit setup`.

I want to add a `mygit install` command that:
1. Copies mygit.exe to a fixed location like C:\Users\<user>\.mygit\bin\mygit.exe
2. Adds that directory to the user's PATH (Windows: registry HKCU\Environment,
   on POSIX: appends to ~/.bashrc or ~/.zshrc)
3. Prints instructions to restart the terminal

The command should be: mygit install
After running it, `mygit review` should work from any directory in a new terminal.

Implement this in C++20. Add commands/install_command.h and install_command.cpp.
Wire it into src/main.cpp. Use <windows.h> RegOpenKeyEx/RegSetValueEx for the
Windows PATH update. Handle POSIX with getenv("HOME") + shell rc file append.
```

---

### 2. FTXUI Rich Terminal UI

```
We are building mygit — a C++20 CLI tool that wraps Git with local LLM-powered
code review. It uses llama.cpp for local inference. The project is built with
CMake + vcpkg. FTXUI is already in vcpkg.json as a dependency.

Current terminal UI (ui/terminal_ui.cpp) uses:
- A basic spinner (std::thread + \r overwrite, ASCII frames | / - \)
- ANSI color codes for the report (red=critical, yellow=medium, dim=low)
- Simple text output with print_report() and print_verdict()

I want to upgrade to FTXUI for:
1. A proper animated spinner using ftxui::animation while the model infers
2. A boxed review report card — bordered panel showing each issue with
   colored severity badge, file:line, and message
3. A summary bar at the bottom (X critical, Y high, Z medium issues)

Keep the same function signatures in terminal_ui.h so no other files change.
The spinner class must still: construct with a message string, stop() clears it,
destructor calls stop(). print_report(const ReviewResult&) and
print_verdict(bool allowed, bool force_ai) stay the same.

Use ftxui::screen, ftxui::Element, ftxui::Renderer. Show the implementation
for terminal_ui.cpp only.
```

---

### 3. spdlog Review Logging

```
We are building mygit — a C++20 CLI tool that wraps Git with local LLM-powered
code review. The project uses CMake + vcpkg. spdlog is already in vcpkg.json.

I want to add structured logging of every review session to logs/mygit.log:
- Timestamp
- Which command ran (review / commit / push)
- Number of issues by severity
- Whether the operation was blocked or allowed
- How long inference took (milliseconds)

Add a logger/ module:
- logger/review_logger.h
- logger/review_logger.cpp

The logger should:
1. Use spdlog rotating_file_sink writing to ~/.mygit/logs/mygit.log
2. Max file size 5MB, keep 3 rotated files
3. Log format: [timestamp] [level] message
4. Expose a single function: log_review(const ReviewResult&, bool allowed,
   const std::string& command, long long inference_ms)

Wire it into commands/review_command.cpp, push_command.cpp, commit_command.cpp
after the review completes. Time the inference with std::chrono.
```

---

### 4. SQLite Memory System (V3)

```
We are building mygit — a C++20 CLI tool that wraps Git with local LLM-powered
code review. The project uses CMake + vcpkg. sqlite3 is already in vcpkg.json.
database/sqlite_manager.cpp exists but only opens a connection — no schema yet.

I want to implement the V3 memory system:

Database location: ~/.mygit/mygit.db

Schema:
  reviews(id, timestamp, branch, commit_hash, files_changed, issues_json, blocked)
  issues(id, review_id, severity, file, line, message)

Implement:
1. Schema creation on first run (CREATE TABLE IF NOT EXISTS)
2. SqliteManager::save_review(const ReviewResult&, const std::string& branch,
   bool blocked) — inserts a review + its issues
3. SqliteManager::get_recent_reviews(int limit) — returns last N reviews
4. A new CLI command: mygit history — prints last 10 reviews in a table
   (timestamp, branch, issues count, blocked Y/N)

The sqlite_manager should use RAII (already does). Add prepared statements
for insert to avoid SQL injection. Wire save_review() into each command after
a review completes.
```

---

### 5. Commit Message Generation (FR-8)

```
We are building mygit — a C++20 CLI tool that wraps Git with local LLM-powered
code review (llama.cpp, Qwen2.5-Coder-1.5B-Instruct-GGUF, GBNF grammar output).

Currently mygit commit -m "message" requires the user to supply a message.
I want to add commit message generation:

mygit commit           (no -m flag)
  -> runs AI review
  -> if passes, generates a conventional commit message from the diff
  -> shows the suggested message and asks: "Use this? [Y/n/e to edit]: "
  -> if Y: runs git commit -m "<generated message>"
  -> if n: runs git commit (opens editor as normal)
  -> if e: opens $EDITOR with the message pre-filled

The generated message should follow Conventional Commits format:
  feat(scope): short description
  fix(auth): correct null check in login handler

Add a new method to ai/prompt_builder.cpp:
  std::string build_commit_message_prompt(const std::string& diff)

And a new method to ai/llama_client.cpp:
  std::string generate_commit_message(const std::string& prompt)

The commit message generation does NOT need grammar constraints — just instruct
the model to return only the commit message string, nothing else.
Update commands/commit_command.cpp to use this flow when no -m is provided.
```

---

### 6. libgit2 Integration (V2)

```
We are building mygit — a C++20 CLI tool. Currently git/git_diff.cpp and
git/git_status.cpp use popen() to shell out to the git CLI. I want to replace
this with libgit2 for reliability and to remove the git-in-PATH requirement.

Add libgit2 to vcpkg.json as "libgit2".
Add find_package(libgit2 CONFIG REQUIRED) to CMakeLists.txt and link libgit2::libgit2.

Rewrite git/git_diff.cpp:
- Use git_repository_open_ext() to find the repo from cwd
- Use git_diff_index_to_workdir() + git_diff_index_to_workdir() for staged changes
  (actually git_diff_head_to_index for staged vs HEAD)
- Use git_diff_to_buf() with GIT_DIFF_FORMAT_PATCH to get the patch string
- RAII wrappers for git_repository*, git_diff* using unique_ptr with custom deleters

Rewrite git/git_status.cpp:
- Use git_repository_head() + git_reference_shorthand() for branch name
- Use git_status_list_new() to check for staged changes

Keep the same public API (GitDiff::get_staged_diff(), GitStatus::get_current_branch(),
GitStatus::has_staged_changes()) so no other files change.
```