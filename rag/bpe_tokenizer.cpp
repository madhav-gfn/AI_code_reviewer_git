#include "rag/bpe_tokenizer.h"

#include <array>
#include <climits>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace mygit::rag {

namespace {

// UTF-8 encodes a single Unicode code point. Only ever called with code
// points < 0x800 here (the GPT-2 byte-to-unicode alphabet tops out around
// 256+68), so the 1- and 2-byte cases are all that's needed.
std::string utf8_encode(int code_point) {
    std::string out;
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
    return out;
}

// The standard GPT-2 "bytes to unicode" table: printable bytes map to
// themselves, everything else (control chars, space, high bytes without a
// printable Latin-1 glyph) maps to a private code point starting at 256.
// This keeps every byte representable as a *visible* token so BPE merges
// operate on a purely textual alphabet - it's what the vocab in
// tokenizer.json is actually built from.
const std::array<std::string, 256>& byte_to_unicode() {
    static const std::array<std::string, 256> table = [] {
        std::array<bool, 256> printable{};
        for (int b = '!'; b <= '~'; ++b) printable[b] = true;
        for (int b = 0xA1; b <= 0xAC; ++b) printable[b] = true;
        for (int b = 0xAE; b <= 0xFF; ++b) printable[b] = true;

        std::array<int, 256> code_point{};
        int next_private = 256;
        for (int b = 0; b < 256; ++b) {
            code_point[b] = printable[b] ? b : next_private++;
        }

        std::array<std::string, 256> result;
        for (int b = 0; b < 256; ++b) {
            result[b] = utf8_encode(code_point[b]);
        }
        return result;
    }();
    return table;
}

bool is_space_byte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool is_word_byte(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;  // >=0x80: keep multi-byte UTF-8 glued together
}

// Approximate pre-tokenizer: splits text into words/numbers/punctuation-runs
// /whitespace-runs, with a single leading space folded into the following
// token (matching the common GPT-2 pretokenizer's " word" convention). See
// the header comment for why this isn't a literal port of the reference
// Unicode regex.
std::vector<std::string> pre_tokenize(const std::string& text) {
    static const char* kContractions[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};

    std::vector<std::string> words;
    size_t i = 0;
    const size_t n = text.size();

    while (i < n) {
        bool matched = false;
        for (const char* c : kContractions) {
            const size_t len = std::strlen(c);
            if (text.compare(i, len, c) == 0) {
                words.push_back(text.substr(i, len));
                i += len;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        const size_t start = i;
        bool leading_space = false;
        if (text[i] == ' ' && i + 1 < n && !is_space_byte(static_cast<unsigned char>(text[i + 1]))) {
            leading_space = true;
            ++i;
        }
        const size_t begin = leading_space ? start : i;
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (is_word_byte(c)) {
            while (i < n && is_word_byte(static_cast<unsigned char>(text[i]))) ++i;
        } else if (std::isdigit(c)) {
            while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
        } else if (is_space_byte(c)) {
            // Whitespace run (not preceded by the single-leading-space case
            // above, since that only fires when the *next* byte isn't
            // whitespace). Source code leans heavily on runs of these
            // (indentation, blank lines), so keep them as their own tokens
            // rather than silently dropping them.
            while (i < n && is_space_byte(static_cast<unsigned char>(text[i]))) ++i;
        } else {
            while (i < n && !is_space_byte(static_cast<unsigned char>(text[i])) &&
                   !is_word_byte(static_cast<unsigned char>(text[i])) &&
                   !std::isdigit(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
        }
        words.push_back(text.substr(begin, i - begin));
    }
    return words;
}

}  // namespace

size_t BpeTokenizer::StringPairHash::operator()(
    const std::pair<std::string, std::string>& p) const noexcept {
    const size_t h1 = std::hash<std::string>{}(p.first);
    const size_t h2 = std::hash<std::string>{}(p.second);
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

std::unique_ptr<BpeTokenizer> BpeTokenizer::load(const std::string& tokenizer_json_path) {
    std::ifstream f(tokenizer_json_path, std::ios::binary);
    if (!f) return nullptr;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception&) {
        return nullptr;
    }

    if (!j.contains("model") || !j["model"].is_object()) return nullptr;
    const nlohmann::json& model = j["model"];
    if (model.value("type", "") != "BPE") return nullptr;
    if (!model.contains("vocab") || !model["vocab"].is_object()) return nullptr;

    std::unique_ptr<BpeTokenizer> tok(new BpeTokenizer());

    for (auto it = model["vocab"].begin(); it != model["vocab"].end(); ++it) {
        if (it.value().is_number_integer()) {
            tok->vocab_.emplace(it.key(), it.value().get<int32_t>());
        }
    }
    if (tok->vocab_.empty()) return nullptr;

    tok->unk_token_ = model.value("unk_token", "");

    if (model.contains("merges") && model["merges"].is_array()) {
        int32_t rank = 0;
        for (const auto& m : model["merges"]) {
            std::string a, b;
            if (m.is_string()) {
                const std::string s = m.get<std::string>();
                const size_t sp = s.find(' ');
                if (sp == std::string::npos) { ++rank; continue; }
                a = s.substr(0, sp);
                b = s.substr(sp + 1);
            } else if (m.is_array() && m.size() == 2 && m[0].is_string() && m[1].is_string()) {
                a = m[0].get<std::string>();
                b = m[1].get<std::string>();
            } else {
                ++rank;
                continue;
            }
            tok->merge_ranks_.emplace(std::make_pair(std::move(a), std::move(b)), rank++);
        }
    }

    return tok;
}

std::vector<std::string> BpeTokenizer::bpe_merge(std::vector<std::string> symbols) const {
    while (symbols.size() >= 2) {
        int best_rank = INT_MAX;
        size_t best_idx = std::string::npos;
        for (size_t i = 0; i + 1 < symbols.size(); ++i) {
            const auto it = merge_ranks_.find({symbols[i], symbols[i + 1]});
            if (it != merge_ranks_.end() && it->second < best_rank) {
                best_rank = it->second;
                best_idx = i;
            }
        }
        if (best_idx == std::string::npos) break;
        symbols[best_idx] += symbols[best_idx + 1];
        symbols.erase(symbols.begin() + static_cast<long>(best_idx) + 1);
    }
    return symbols;
}

std::vector<int32_t> BpeTokenizer::encode(const std::string& text) const {
    std::vector<int32_t> ids;
    const auto& byte_map = byte_to_unicode();

    for (const std::string& word : pre_tokenize(text)) {
        std::vector<std::string> symbols;
        symbols.reserve(word.size());
        for (unsigned char b : word) {
            symbols.push_back(byte_map[b]);
        }

        for (const std::string& sym : bpe_merge(std::move(symbols))) {
            const auto it = vocab_.find(sym);
            if (it != vocab_.end()) {
                ids.push_back(it->second);
            } else if (const auto uit = vocab_.find(unk_token_); uit != vocab_.end()) {
                ids.push_back(uit->second);
            }
        }
    }
    return ids;
}

}  // namespace mygit::rag
