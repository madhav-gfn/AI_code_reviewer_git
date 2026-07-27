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
//
// For non-code files (Markdown, YAML, etc.) that don't have a Tree-sitter
// grammar, the universal text chunker produces CodeUnits with unit_name set
// to "<file>:L<start>-L<end>" (architecture review #3 bullet 2).
struct CodeUnit {
    std::string file_path;
    std::string unit_name;  // e.g. "DecisionEngine::should_allow" or "README.md:L1-L40"
    std::string content;    // raw source text of the unit
};

// True if `file_path`'s extension indicates a source file whose language has
// a Tree-sitter grammar compiled in (C/C++, Python, JavaScript, TypeScript,
// Go, Rust, Java). Exposed so RagOrchestrator can quickly check whether a
// tracked file will produce AST-level CodeUnits.
bool is_parseable_file(const std::string& file_path);

// Backward-compatible alias — existing callers (tests, rag_orchestrator)
// that check for C/C++ specifically can still use this.
inline bool is_cpp_source_file(const std::string& file_path) {
    // Only returns true for .c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx
    // (implemented in code_parser.cpp as a subset of is_parseable_file).
    extern bool is_cpp_source_file_impl(const std::string& file_path);
    return is_cpp_source_file_impl(file_path);
}

// True if `file_path` is a plain text file that should be indexed via the
// sliding-window text chunker when no Tree-sitter grammar matches. Returns
// false for known binary extensions (.png, .exe, etc.).
bool is_text_chunkable_file(const std::string& file_path);

// Returns true if `file_path` is indexable by the RAG pipeline at all:
// either via a Tree-sitter grammar or via the text chunker.
inline bool is_indexable_file(const std::string& file_path) {
    return is_parseable_file(file_path) || is_text_chunkable_file(file_path);
}

// Parses source files into CodeUnits using the appropriate Tree-sitter
// grammar, or falls back to sliding-window text chunking for non-code files.
// Kept separate from Embedder/VectorStore so grammar/AST details don't leak
// into the rest of the RAG pipeline.
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
    // CodeUnit::file_path). For grammar-supported files, returns one
    // CodeUnit per function, method, class, or struct found. For other text
    // files, returns sliding-window chunks. Returns an empty vector for
    // binary files or files that fail to read — never throws.
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
