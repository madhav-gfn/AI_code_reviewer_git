#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mygit::rag {

// A byte-level BPE tokenizer compatible with HuggingFace `tokenizer.json`
// files (the format used by Qwen2.5-Coder / Qodo-Embed and most modern
// embedding models). Loads the vocab + merge table straight from the
// model's own tokenizer.json so token IDs match what the ONNX model was
// trained on - a naive whitespace/word-piece splitter would produce IDs the
// model was never trained to interpret, i.e. garbage embeddings.
//
// Pre-tokenization uses an ASCII-focused approximation of the reference
// GPT-2-style byte-level regex (splits input into runs of letters/digits/
// punctuation/whitespace) rather than full Unicode \p{L}/\p{N} matching -
// std::regex has no Unicode property support and pulling in ICU just for
// this felt like the wrong tradeoff. This is exact for ASCII text, which
// covers the overwhelming majority of source code and diffs; non-ASCII
// identifiers/comments still tokenize via the byte-level fallback, just not
// always identically to the reference implementation's word boundaries.
class BpeTokenizer {
public:
    // Returns nullptr on any load failure (missing file, malformed JSON,
    // unsupported model type, empty vocab) - callers should treat that as
    // "tokenizer unavailable" and fall back accordingly (see Embedder).
    static std::unique_ptr<BpeTokenizer> load(const std::string& tokenizer_json_path);

    // Encodes `text` into model vocabulary token IDs. Symbols with no vocab
    // entry fall back to the model's unk_token, if any; otherwise they're
    // dropped.
    std::vector<int32_t> encode(const std::string& text) const;

private:
    BpeTokenizer() = default;

    std::vector<std::string> bpe_merge(std::vector<std::string> symbols) const;

    struct StringPairHash {
        size_t operator()(const std::pair<std::string, std::string>& p) const noexcept;
    };

    std::unordered_map<std::string, int32_t> vocab_;
    std::unordered_map<std::pair<std::string, std::string>, int32_t, StringPairHash> merge_ranks_;
    std::string unk_token_;
};

}  // namespace mygit::rag
