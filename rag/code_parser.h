#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mygit::rag {

// A single logical chunk of source code extracted from a Tree-sitter AST: a
// function/method definition, or a class/struct declaration. This is the
// unit of retrieval for the RAG pipeline (finer-grained than whole files,
// which keeps embeddings focused and retrieval accurate - see
// docs/architecture_review.md bottleneck #3).
struct CodeUnit {
    std::string file_path;
    std::string unit_name;  // e.g. "DecisionEngine::should_allow"
    std::string content;    // raw source text of the unit
};

// True if `file_path`'s extension indicates a C/C++ source or header file
// (.c, .cc, .cpp, .cxx, .h, .hpp, .hh, .hxx) - the only files CodeParser
// will actually parse. Exposed so RagOrchestrator can skip non-C++ tracked
// files before even calling into the parser.
bool is_cpp_source_file(const std::string& file_path);

// Parses C/C++ source into CodeUnits using tree-sitter-cpp. Kept separate
// from Embedder/VectorStore so grammar/AST details don't leak into the rest
// of the RAG pipeline.
//
// tree_sitter/api.h is intentionally kept out of this header (Pimpl) per the
// project's header-hygiene rule - only code_parser.cpp depends on it.
class CodeParser {
public:
    CodeParser();
    ~CodeParser();

    CodeParser(const CodeParser&) = delete;
    CodeParser& operator=(const CodeParser&) = delete;

    // Reads and parses `file_path` from disk (also used as
    // CodeUnit::file_path). Returns one CodeUnit per function, method,
    // class, or struct found. Returns an empty vector for files that aren't
    // C/C++ (by extension) or that fail to read/parse - never throws.
    std::vector<CodeUnit> parse_file(const std::string& file_path) const;

    // Same as parse_file, but parses already-loaded `source` text instead of
    // reading from disk. `display_path` is stored as CodeUnit::file_path.
    std::vector<CodeUnit> parse_source(const std::string& display_path,
                                        const std::string& source) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mygit::rag
