#pragma once

#include <string>

namespace mygit::ai {

// Wraps inference calls to a local llama.cpp model (FR-3, NFR Privacy).
//
// NOT YET WIRED UP: this is a stub that returns a placeholder response so
// the rest of the pipeline (parsing, decision engine, reporting) can be
// built and tested independently. Swap the body of review() for an actual
// llama.cpp call (see external/llama.cpp + models/) as the next step.
class LlamaClient {
public:
    // Sends a prompt to the local model and returns its raw text response.
    std::string review(const std::string& prompt) const;
};

}  // namespace mygit::ai
