#pragma once

namespace mygit::commands {

// Runs `mygit init`: builds (or updates) the RAG embedding index for the
// current Git repository. The index is stored per-repo inside `.git/mygit/`
// (rag.db + rag.index), while the heavy ONNX model weights stay global at
// `~/.mygit/models/`. Returns 0 on success, 1 on failure.
int run_init();

}  // namespace mygit::commands
