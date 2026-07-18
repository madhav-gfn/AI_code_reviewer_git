I Taught Git to Say No

I built a fully local AI code reviewer in C++20 by removing hope from the pipeline — one subsystem at a time.

The first version died on a trailing comma.

I'd ask a local LLM to review my staged diff and return JSON. Most of the time it did. Sometimes it wrapped the JSON in markdown fences. Sometimes it added a cheerful sentence before the opening brace. Once it just... stopped, mid-array, like it got bored. My parser crashed, my pre-commit check silently passed, and I realized the entire tool was built on hope.

That became the design rule for everything that followed: **find every place the pipeline hopes, and replace it with a place the pipeline knows.** mygit — a C++20 CLI wrapper around Git that runs an LLM code review before every commit and push, entirely on your own hardware — is what you get after applying that rule about six times in a row.

Here's the pitch in one paragraph: `mygit commit` diffs your staged changes via libgit2, filters the diff, retrieves related code from a FAISS index of your repo, sends it all to a llama.cpp model whose output is grammar-constrained to a strict JSON schema, and blocks the commit if anything comes back `critical`. No API keys, no network calls, no monthly bill. Your code never leaves your machine — which, if you've read your cloud code-review vendor's data retention policy lately, you may find newly appealing.

## Hope #1: "please return valid JSON"

Prompt engineering is a negotiation. A grammar is a law.

Instead of begging the model for valid JSON and writing defensive parsing for the days it disobeys, mygit hands llama.cpp a GBNF grammar (GGML's BNF dialect) and attaches it as a sampler. At every decoding step, tokens that would violate the schema are masked out before sampling even happens. The model isn't instructed to produce this shape — it is *incapable* of producing anything else:

```
root        ::= "{" ws "\"safe\":" ws boolean "," ws "\"issues\":" ws issue-array ws "}" ws
issue-array ::= "[" ws ( issue ("," ws issue)* )? "]" ws
```

Every issue carries a severity (`critical`, `high`, `medium`, `low`), a file, a line, and a message. The sampler chain is grammar-then-greedy, so the same diff produces the same verdict run after run. The JSON parser downstream has never seen malformed input, because malformed input is no longer a thing that can exist.

The decision engine that consumes this is deliberately boring: `critical` blocks the commit or push, everything else reports and passes through. `--force-ai` overrides the block, because a tool that can't be overruled is a tool that gets uninstalled.

The model doing the reviewing is Qwen2.5-Coder-1.5B-Instruct at Q4_K_M quantization — about 1.12 GB on disk, small enough to run on hardware that isn't trying to heat your apartment. llama.cpp offloads layers to GPU via CUDA or Vulkan when available and falls back to CPU when not. The chat template comes from the GGUF file itself via `llama_chat_apply_template()`, so swapping model families doesn't mean rewriting prompt formatting.

## Hope #2: "surely parsing `git diff` output is fine"

Early mygit shelled out to git with `popen` and parsed the text. This works right up until it doesn't — different git versions, different locales, Windows line endings, a filename with a space in it. Text parsing of a CLI meant for humans is hope wearing a trench coat.

So the shell integration got ripped out entirely. mygit links libgit2 and talks to the Git object database directly: `git_diff_head_to_index` for staged changes, status and branch queries against the same in-memory structures git itself uses. Every C-style handle — `git_repository*`, `git_diff*`, `git_tree*` — lives inside a `std::unique_ptr` with a custom deleter, so the C API's manual memory management is invisible above the wrapper layer.

Going native also fixed a subtler bug: renames. A textual diff shows a moved file as a full deletion plus a full addition, so the model would confidently report that you deleted 400 lines and wrote 400 suspiciously similar new ones. mygit runs libgit2's rename detection (`git_diff_find_similar`, content-similarity based) before anything reaches the model. Pure renames are dropped from analysis entirely; renames with edits contribute only the actual edits.

On top of that sits a diff filter with hard limits — 50 KB total, 20 KB per file, 30 files max — that drops binary files and rejects diffs too large to review meaningfully. A model reviewing a 2 MB lockfile change isn't reviewing anything; it's summarizing noise. Better to refuse loudly than approve blindly.

## Hope #3: "the user won't mind waiting"

Local inference has a dirty secret: for small diffs, loading the model takes longer than running it. Spinning up a fresh process per command meant paying the weight-loading tax on every single `mygit review`. That's the kind of latency that gets a tool quietly removed from someone's workflow within a week.

mygit attacks this from four directions at once.

**A resident daemon.** The first command spawns a detached background process that loads the model once and keeps it in memory, exposing `/review`, `/commit`, `/health`, and `/shutdown` over local HTTP. The CLI auto-starts it if it's not running and polls `/health` every 500 ms until it's ready. Cold start happens once per boot, not once per commit.

**Prefix KV caching.** The system prompt and review instructions are identical across every request — only the diff changes. Prompts are split into a static prefix and a dynamic suffix, and the transformer's KV state for the prefix is computed once and reused. Because the daemon outlives any single CLI invocation, that cached prefix survives *across commands*: your third `mygit commit` of the day starts decoding at the diff, not at token zero.

**Per-file batching.** Rather than one monolithic prompt containing every changed file, each file is reviewed independently — sharing the cached prefix — and a lightweight aggregation pass merges the per-file verdicts into one repository-level result. Smaller contexts also mean less context dilution; a 1.5B model reasons noticeably better about one file than about eight concatenated ones.

**Content-addressed caching.** Every per-file review is stored in SQLite keyed by a fingerprint of the file path plus its patch text. Re-review a commit where only one of five files changed, and four verdicts come straight from the cache with zero inference. And database writes — reviews, issues, history for the `mygit history` table — happen on a background writer thread, so the terminal prompt returns the instant the verdict does, not after SQLite finishes fsyncing.

## Hope #4: "the diff is enough context"

Show a reviewer only the `+` and `-` lines and you get plausible nonsense. If a diff changes a call to `should_allow()`, the model has never seen `should_allow()`'s body. It's reviewing a conversation it walked into halfway through.

The RAG pipeline fixes this, and it's the most infrastructure-dense part of the project:

- **Tree-sitter** parses every tracked C/C++ file into logical code units — functions, methods, classes, structs — rather than whole files. A unit like `DecisionEngine::should_allow` embeds as one focused vector instead of drowning in its 400-line neighbor.
- A from-scratch **byte-level BPE tokenizer**, compatible with HuggingFace `tokenizer.json` files, feeds an embedding model running under **ONNX Runtime**. The tested model is Qodo-Embed-1-1.5B, exported to ONNX by a script in the repo; the embedder resolves tensor names from the ONNX graph itself, so it isn't welded to one model's export conventions.
- Vectors land in a **FAISS `IndexHNSWFlat`**, searched by cosine similarity via inner product over L2-normalized embeddings. HNSW has no per-id deletion, so removed files are tombstoned and the graph is lazily rebuilt once tombstones cross a threshold — the classic append-only-index workaround, handled where you'd forget to handle it.
- **Indexing is incremental by content hash**, not by commit. On each `mygit commit`, only files whose bytes actually changed get re-parsed and re-embedded — staged-but-uncommitted edits included — and files no longer tracked by git get purged. Index maintenance stays near-constant regardless of repo size.

At commit time, the diff itself is embedded, the top-3 nearest code units come back from FAISS, and they're prepended to the prompt. The model finally gets to read the function it's judging a call to.

My favorite part is what happens when none of this is set up: nothing. If the ONNX model or tokenizer is missing, `RagOrchestrator::available()` returns false and every RAG call becomes a safe no-op. No crash, no setup wizard, one warning line on stderr. The same rule the LLM output got — never depend on hope — applied to the feature's own availability. Graceful degradation is a feature you build, not a property you assume.

## The boss fight: building ONNX Runtime with CUDA 13 on Windows

CPU embeddings work fine. GPU embeddings on a bleeding-edge toolchain cost me a vcpkg overlay port and several evenings, and the scars are checked into the repo as documentation. A sampler:

- ONNX Runtime's default `CMAKE_CUDA_ARCHITECTURES` list includes compute capabilities that CUDA 13 dropped. The overlay pins the build to the actual GPU's architecture (`86-real` for mine — edit to match yours).
- CUDA 13's CCCL headers require MSVC's conforming preprocessor, so the port adds `/Zc:preprocessor`. It also defines `__NV_NO_VECTOR_DEPRECATION_DIAG` — NVIDIA's own suppression for vector types CUDA 13 soft-deprecated but onnxruntime 1.23.2 still uses.
- A preview MSVC optimizer decided an explicitly instantiated template was "unreferenced" and discarded it, breaking linking. The fix is a one-line `extern template` patch.
- `onnxruntime_DISABLE_CONTRIB_OPS=ON`, because mygit's embedder only uses standard ONNX ops and the contrib ops — flash-attention, MoE kernels, CUTLASS — are RAM-hungry enough per translation unit to OOM a 16 GB machine mid-build.
- And Windows' 260-character `MAX_PATH` limit, which onnxruntime's long checked-in filenames plus vcpkg's deep build trees exceed comfortably. The workaround is redirecting vcpkg's install root to something like `E:\vi`. In 2026. You know exactly how I felt typing that.

If the pipeline fails anyway — driver mismatch, half-installed cuDNN, cosmic rays — the embedder catches the CUDA session failure and silently falls back to CPU. Hope removed, again.

## Where this goes

The roadmap keeps pulling the same thread: quantization benchmarks across INT8/INT4/FP16 for the embedding side, task-specific model routing (a small model writes your commit message, a bigger one reviews your pointer arithmetic), and eventually patch generation — the review that doesn't just flag the null dereference but hands you a unified diff that fixes it, previewed and applied on approval.

But the core loop already runs, and it runs the way it was designed to: `mygit commit` reviews your staged changes against your own repo's context, writes you a Conventional Commit message (`Use this? [Y/n/e]`), and refuses to proceed past a critical issue unless you explicitly overrule it.

That first trailing comma never happened again — not because the model got smarter, but because I stopped asking it nicely. Every guarantee in this project works the same way. If your pipeline depends on an LLM behaving, you don't have a pipeline. You have a hope with a progress spinner.
