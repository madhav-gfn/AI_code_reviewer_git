#include "ai/llama_client.h"

namespace mygit::ai {

std::string LlamaClient::review(const std::string& /*prompt*/) const {
    // TODO: replace with a real llama.cpp call once the model runtime is
    // integrated. Returning a fixed, schema-valid response keeps the
    // downstream pipeline (json_parser -> decision_engine -> reports)
    // testable in isolation until then.
    return R"({"safe": true, "issues": []})";
}

}  // namespace mygit::ai
