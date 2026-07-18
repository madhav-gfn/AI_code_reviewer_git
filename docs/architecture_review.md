# Architecture Review & Kickass Roadmap

Alright, honest look at `mygit` as it stands today..... While the scaffolding is solid and the shift to `libgit2` and local GBNF `llama.cpp` inference puts it at a better place, but there are glaring inefficiencies and many missed opportunities. 

I want this to be a true "kickass" engineering marvel, here is exactly what needs to be torn down, optimized, and implemented.

---

### Update:

The five bottlenecks — all five addressed:

# : 1
Item: Cold start penalty
Status: ✅ Done, both proposed fixes: the HTTP daemon (mygit daemon start/stop/status, auto-spawned on
first cache miss, wired into commit_command.cpp) and the content-addressed review cache
(hash_diff_content in SQLite — unchanged file diffs never re-run inference)
────────────────────────────────────────
# : 2
Item: Synchronous DB writes
Status: ✅ Done — database/async_db_writer.cpp, background writer thread
────────────────────────────────────────
# : 3
Item: Zero RAG
Status: ✅ Built exactly as specced: Tree-sitter chunking into code units, ONNX embedder (Qodo-Embed
tested), FAISS index, incremental re-indexing by content hash. Caveat: it's dormant until you drop the
 model files in — available() is false out of the box
────────────────────────────────────────
# : 4
Item: Rename handling
Status: ✅ Done — git_diff_find_similar detection, pure renames skipped, renamed-with-edits keeps only
the edits
────────────────────────────────────────
# : 5
Item: Diff acceptance limits
Status: 🟡 Mostly — binary files, oversized files (20KB), oversized totals (50KB), too many files (30)
all rejected. The doc also mentions skipping documentation/md files, which the filter doesn't
specifically do

Level 1 (pipeline acceleration) — done. Prefix KV caching (SplitPrompt, cached across CLI invocations via the daemon), per-file batched reviews with the aggregation pass, incremental/staged-only processing, and context reduction via the filter. One gap: the doc says per-file batches run "in parallel using CUDA" — they're reviewed sequentially through one model context, just with the shared prefix cached.

Level 2 (model optimization) — not done. No quantization benchmarking, no task-specific model routing, no ONNX graph-optimization passes. The "static prompt" idea is partially covered by prefix caching, but the rest is still roadmap.

Level 3 (agentic patch generation) — not done. The agents/ directory is an empty .gitkeep.

The closing verdict listed three priorities — KV prefix caching, IPC daemon, local semantic search — and all three shipped. So the honest summary: every bottleneck and all of Level 1 is implemented; Levels 2 and 3 are the remaining frontier.

## The Ugly: Bottlenecks

### 1. The Cold Start Penalty
Right now, every time the user runs `mygit review` or `mygit commit`, `LlamaClient` instantiates the context, loads the weights (even if using mmap), and then shuts down. 
**The flaw:** If we do a review, and then right after do a commit, you are incurring the load penalty twice. Model load time often dominates generation time for small diffs.
**The fix:** A daemonized architecture. `mygit` should spawn a lightweight background process that holds the model in VRAM persistently. The CLI simply sends IPC/RPC requests to the daemon. This drops perceived latency to literally 0ms.
OR
we can additionally add some type of caching mechanism like kv-cache caching so that we can reduce the load time. and some additional things like if the diff has been reviewed we don't send it to the model again so lets say we have 3 diffs which are already reviewed and now i have made 2 more changes one in different file and one in one of these 3 files so now only 2 diffs will be compared

### 2. Synchronous Database Operations
Every review blocks the main thread to perform SQLite transactions. While SQLite is fast, doing I/O synchronously while the user is waiting for their terminal prompt to return is bad UX.
**The fix:** Offload all database writes to a background thread or process them asynchronously so the CLI exits the millisecond the LLM finishes generating the verdict.

### 3. Blind Context (Zero RAG)
The AI is currently operating in a vacuum. It only sees the raw diff (the `+` and `-` lines). If I change a function call in `main.cpp`, the model has no idea what that function actually does because it hasn't read the header file.
**The fix:** Repository-Aware Context Retrieval for Commit Messages

I will implement a lightweight RAG pipeline to generate repository-aware commit messages. The repository will be parsed using Tree-sitter and chunked into logical code units (functions, methods, classes, and structs) instead of entire files to improve retrieval accuracy. Each code unit will be embedded locally using Qodo Embed 1.5B with ONNX GPU Runtime and indexed in FAISS.

To optimize indexing, I will implement incremental indexing, re-parsing and re-embedding only modified code units after each commit instead of rebuilding the entire vector index. During commit message generation, I will embed the Git diff, retrieve the top-k semantically relevant code units from FAISS, and include this repository context alongside the diff in the LLM prompt, enabling more accurate and context-aware commit messages while keeping the system fully local and efficient.


### 4. Incorrect Handling of File Renames

The current pipeline treats a file rename or move as a file deletion followed by a new file creation. Consequently, the entire contents of the relocated file are analyzed and included in commit message generation, even when the file itself is unchanged. This increases prompt size, wastes computation, and often results in misleading commit messages that describe a file as being deleted and recreated instead of simply moved.

**The fix**: Rename-Aware Change Detection

I will implement rename detection using libgit2 (git_diff_find_similar()) to identify file moves and renames based on content similarity before processing the diff. Pure file relocations will be classified as rename operations and excluded from content analysis. If a renamed file also contains code modifications, only the actual changes will be analyzed while preserving the rename metadata. This reduces unnecessary context, improves processing efficiency, and generates commit messages that accurately describe repository reorganizations instead of false file additions and deletions.
---
### 5. all acceptance

it should not accept the diffs if there are too many or too big diff or the diff is about some documentation file which is a binary non text or code or md file

## 🚀 The Roadmap: Advanced CUDA & Model Optimization

If we want to push this to absolute state-of-the-art performance, we need to dive deep into pipeline optimization and model editing.

### Level 1: Pipeline Acceleration

- **Prefix KV Caching:** The system prompt, review guidelines, and GBNF grammar remain constant across every inference. I will cache their KV states after the first run so subsequent requests begin directly from the Git diff, eliminating repeated prompt evaluation and reducing inference latency.

- **Batched Per-File Processing:** Instead of sending a large multi-file diff to the model, I will split the staged changes into individual file-level batches. Each file will be reviewed independently in parallel using CUDA, after which a lightweight aggregation pass will combine the results into a single repository-level review with overall severity and summary. This improves both inference speed and reasoning quality by reducing context dilution.

- **Incremental Processing:** Rather than analyzing the entire repository on every execution, I will process only staged files and modified code regions. Combined with Git-aware change detection, this minimizes unnecessary inference and keeps review times nearly constant regardless of repository size.

- **Prefix-Aware Context Reduction:** Before inference, the pipeline will filter irrelevant context by excluding unchanged files, pure file renames, and unrelated code from the prompt. Combined with repository-aware retrieval, this keeps the context window focused on only the information required for the current review, reducing token count while improving response quality.


### Level 2: Model Optimization & Architectural Enhancements

- **Static Prompt Graph Optimization:** Since the prompt structure is deterministic (system prompt → retrieved context → Git diff), I will optimize prompt construction by separating static and dynamic components. Static prefixes will be cached while only the Git diff and retrieved context are processed at runtime, reducing prompt evaluation overhead.

- **Mixed-Precision & Quantization Optimization:** I will benchmark multiple quantization formats (INT8, INT4, FP16) using ONNX Runtime to identify the optimal trade-off between inference speed and review quality. Different tasks, such as commit message generation and code review, may use different optimized model variants.

- **Task-Specific Model Routing:** Instead of relying on a single model for every operation, I will introduce a lightweight routing layer that selects the most suitable local model based on the requested task. Smaller models can generate commit messages, while larger reasoning models are reserved for complex code reviews, reducing average inference latency.

- **Graph-Level ONNX Optimization:** Before deployment, the model graph will be optimized using ONNX Runtime's graph optimization passes, including operator fusion, constant folding, memory reuse, and kernel selection. This minimizes runtime overhead and maximizes GPU utilization without modifying model behavior.
### Level 3: The "Agentic" Shift (V4 / V5)
- **Patch Generation & Safe Auto-Fix:** Instead of only reporting issues, `mygit` will generate Git-compatible unified patches for detected problems. The generated patch can be previewed, validated, and applied automatically upon user approval, enabling one-command fixes while preserving the standard Git workflow.
---

## The Verdict

we have built an incredibly solid engine by tying `libgit2` and `llama.cpp` together natively. The next step is moving from a **CLI Tool** to an **AI Daemon**. 

1. **Implement KV Prefix Caching** (Immediate speedup).
2. **Move to an IPC Daemon model** (Zero cold starts).
3. **Build local semantic search** (Context-aware reviews).

Let's build it.
