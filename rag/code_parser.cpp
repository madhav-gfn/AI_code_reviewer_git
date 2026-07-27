#include "rag/code_parser.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <tree_sitter/api.h>

namespace mygit::rag {

// ---------------------------------------------------------------------------
// Tree-sitter grammar entry points (compiled as separate static libs via
// CMake FetchContent - see CMakeLists.txt). Not exposed through any
// vcpkg-installed header, so we declare the C symbols ourselves.
// ---------------------------------------------------------------------------
extern "C" {
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_go(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_java(void);
}

namespace {

// ---------------------------------------------------------------------------
// Binary extension set (shared with diff_filter - kept in sync manually since
// both modules are leaf-level).
// ---------------------------------------------------------------------------
const std::set<std::string>& binary_extensions() {
    static const std::set<std::string> exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".svg", ".pdf", ".zip",
        ".tar", ".gz",  ".exe",  ".dll", ".so",  ".dylib", ".o", ".obj", ".lib",
        ".a",   ".woff", ".woff2", ".ttf", ".eot", ".mp3", ".mp4", ".mov", ".avi",
        ".wav", ".webp", ".webm", ".flac", ".bin", ".dat", ".db", ".sqlite",
        ".pyc", ".pyo", ".class", ".jar", ".war",
    };
    return exts;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string get_extension(const std::string& file_path) {
    const auto dot = file_path.find_last_of('.');
    if (dot == std::string::npos) return "";
    return to_lower(file_path.substr(dot));
}

// ---------------------------------------------------------------------------
// Language ID — used to select grammar + AST walking strategy
// ---------------------------------------------------------------------------
enum class LangId {
    Cpp,
    Python,
    JavaScript,
    TypeScript,
    Go,
    Rust,
    Java,
    Unknown,
};

LangId detect_language(const std::string& ext) {
    // C/C++
    if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
        ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx")
        return LangId::Cpp;
    // Python
    if (ext == ".py" || ext == ".pyi") return LangId::Python;
    // JavaScript
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs")
        return LangId::JavaScript;
    // TypeScript
    if (ext == ".ts" || ext == ".tsx") return LangId::TypeScript;
    // Go
    if (ext == ".go") return LangId::Go;
    // Rust
    if (ext == ".rs") return LangId::Rust;
    // Java
    if (ext == ".java") return LangId::Java;

    return LangId::Unknown;
}

const TSLanguage* get_language(LangId id) {
    switch (id) {
        case LangId::Cpp:        return tree_sitter_cpp();
        case LangId::Python:     return tree_sitter_python();
        case LangId::JavaScript: return tree_sitter_javascript();
        case LangId::TypeScript: return tree_sitter_typescript();
        case LangId::Go:         return tree_sitter_go();
        case LangId::Rust:       return tree_sitter_rust();
        case LangId::Java:       return tree_sitter_java();
        default:                 return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Shared AST helpers
// ---------------------------------------------------------------------------
std::string node_text(TSNode node, const std::string& source) {
    if (ts_node_is_null(node)) return {};
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start > end || end > source.size()) return {};
    return source.substr(start, end - start);
}

TSNode field(TSNode node, const char* name) {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}

// ---------------------------------------------------------------------------
// C/C++ AST walker (original logic, unchanged)
// ---------------------------------------------------------------------------

// Descends through wrapper declarator nodes (pointer_declarator,
// reference_declarator, function_declarator, ...) via their "declarator"
// field until it reaches a name-bearing leaf (identifier, field_identifier,
// qualified_identifier, destructor_name, operator_name) and returns its
// text. Handles qualified_identifier's "name" field so out-of-line
// definitions like "Foo::bar" resolve correctly.
std::string extract_declarator_name(TSNode node, const std::string& source) {
    TSNode current = node;
    for (int guard = 0; guard < 64 && !ts_node_is_null(current); ++guard) {
        const std::string type = ts_node_type(current);
        if (type == "identifier" || type == "field_identifier" ||
            type == "qualified_identifier" || type == "destructor_name" ||
            type == "operator_name") {
            return node_text(current, source);
        }

        TSNode next = field(current, "declarator");
        if (!ts_node_is_null(next)) {
            current = next;
            continue;
        }

        TSNode name_field = field(current, "name");
        if (!ts_node_is_null(name_field)) {
            current = name_field;
            continue;
        }

        break;
    }
    return {};
}

// Recursively walks the AST collecting function/method definitions and
// class/struct declarations as CodeUnits. `class_stack` tracks enclosing
// class/struct names so inline method definitions get a qualified name
// (e.g. "DecisionEngine::should_allow") even though the AST only has the
// bare method name at that point.
void walk_cpp(TSNode node, const std::string& source, const std::string& file_path,
              std::vector<std::string>& class_stack, std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);
    bool pushed_class = false;

    if (type == "class_specifier" || type == "struct_specifier") {
        const std::string class_name = node_text(field(node, "name"), source);
        if (!class_name.empty()) {
            out.push_back(CodeUnit{file_path, class_name, node_text(node, source)});
            class_stack.push_back(class_name);
            pushed_class = true;
        }
    } else if (type == "function_definition") {
        TSNode declarator = field(node, "declarator");
        const std::string name =
            ts_node_is_null(declarator) ? std::string() : extract_declarator_name(declarator, source);
        if (!name.empty()) {
            // Out-of-line definitions (e.g. "Foo::bar") already carry their
            // own qualification; don't double-prefix those with the
            // enclosing scope (which, for out-of-line defs, is just the
            // translation unit anyway).
            std::string qualified = name;
            if (name.find("::") == std::string::npos && !class_stack.empty()) {
                qualified = class_stack.back() + "::" + name;
            }
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_cpp(ts_node_child(node, i), source, file_path, class_stack, out);
    }

    if (pushed_class) class_stack.pop_back();
}

// ---------------------------------------------------------------------------
// Python AST walker
// ---------------------------------------------------------------------------
void walk_python(TSNode node, const std::string& source, const std::string& file_path,
                 std::vector<std::string>& scope_stack, std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);
    bool pushed_scope = false;

    if (type == "class_definition") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            out.push_back(CodeUnit{file_path, name, node_text(node, source)});
            scope_stack.push_back(name);
            pushed_scope = true;
        }
    } else if (type == "function_definition") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            std::string qualified = name;
            if (!scope_stack.empty()) {
                qualified = scope_stack.back() + "." + name;
            }
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_python(ts_node_child(node, i), source, file_path, scope_stack, out);
    }

    if (pushed_scope) scope_stack.pop_back();
}

// ---------------------------------------------------------------------------
// JavaScript / TypeScript AST walker
// ---------------------------------------------------------------------------
void walk_js(TSNode node, const std::string& source, const std::string& file_path,
             std::vector<std::string>& scope_stack, std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);
    bool pushed_scope = false;

    if (type == "class_declaration") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            out.push_back(CodeUnit{file_path, name, node_text(node, source)});
            scope_stack.push_back(name);
            pushed_scope = true;
        }
    } else if (type == "function_declaration" || type == "method_definition") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            std::string qualified = name;
            if (!scope_stack.empty()) {
                qualified = scope_stack.back() + "." + name;
            }
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    } else if (type == "arrow_function" || type == "function") {
        // Named variable assignment: const foo = () => { ... }
        // The parent is typically a variable_declarator with a "name" field.
        TSNode parent = ts_node_parent(node);
        if (!ts_node_is_null(parent)) {
            const std::string parent_type = ts_node_type(parent);
            if (parent_type == "variable_declarator") {
                const std::string name = node_text(field(parent, "name"), source);
                if (!name.empty()) {
                    out.push_back(CodeUnit{file_path, name, node_text(parent, source)});
                }
            }
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_js(ts_node_child(node, i), source, file_path, scope_stack, out);
    }

    if (pushed_scope) scope_stack.pop_back();
}

// ---------------------------------------------------------------------------
// Go AST walker
// ---------------------------------------------------------------------------
void walk_go(TSNode node, const std::string& source, const std::string& file_path,
             std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);

    if (type == "function_declaration") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            out.push_back(CodeUnit{file_path, name, node_text(node, source)});
        }
    } else if (type == "method_declaration") {
        const std::string name = node_text(field(node, "name"), source);
        // Try to get the receiver type for a qualified name like "Foo.Bar".
        std::string qualified = name;
        TSNode receiver = field(node, "receiver");
        if (!ts_node_is_null(receiver)) {
            // Go receiver is a parameter_list containing a type.
            const std::string recv_text = node_text(receiver, source);
            // Extract just the type name from "(f *Foo)" or "(f Foo)"
            for (uint32_t i = 0; i < ts_node_named_child_count(receiver); ++i) {
                TSNode param = ts_node_named_child(receiver, i);
                TSNode param_type = field(param, "type");
                if (!ts_node_is_null(param_type)) {
                    std::string type_name = node_text(param_type, source);
                    // Strip pointer prefix
                    if (!type_name.empty() && type_name[0] == '*') {
                        type_name = type_name.substr(1);
                    }
                    if (!type_name.empty() && !name.empty()) {
                        qualified = type_name + "." + name;
                    }
                    break;
                }
            }
        }
        if (!qualified.empty()) {
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    } else if (type == "type_declaration") {
        out.push_back(CodeUnit{file_path, node_text(field(node, "name"), source),
                               node_text(node, source)});
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_go(ts_node_child(node, i), source, file_path, out);
    }
}

// ---------------------------------------------------------------------------
// Rust AST walker
// ---------------------------------------------------------------------------
void walk_rust(TSNode node, const std::string& source, const std::string& file_path,
               std::vector<std::string>& scope_stack, std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);
    bool pushed_scope = false;

    if (type == "struct_item" || type == "enum_item" || type == "trait_item") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            out.push_back(CodeUnit{file_path, name, node_text(node, source)});
            scope_stack.push_back(name);
            pushed_scope = true;
        }
    } else if (type == "impl_item") {
        const std::string name = node_text(field(node, "type"), source);
        if (!name.empty()) {
            scope_stack.push_back(name);
            pushed_scope = true;
        }
    } else if (type == "function_item") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            std::string qualified = name;
            if (!scope_stack.empty()) {
                qualified = scope_stack.back() + "::" + name;
            }
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_rust(ts_node_child(node, i), source, file_path, scope_stack, out);
    }

    if (pushed_scope) scope_stack.pop_back();
}

// ---------------------------------------------------------------------------
// Java AST walker
// ---------------------------------------------------------------------------
void walk_java(TSNode node, const std::string& source, const std::string& file_path,
               std::vector<std::string>& scope_stack, std::vector<CodeUnit>& out) {
    const std::string type = ts_node_type(node);
    bool pushed_scope = false;

    if (type == "class_declaration" || type == "interface_declaration" ||
        type == "enum_declaration") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            out.push_back(CodeUnit{file_path, name, node_text(node, source)});
            scope_stack.push_back(name);
            pushed_scope = true;
        }
    } else if (type == "method_declaration" || type == "constructor_declaration") {
        const std::string name = node_text(field(node, "name"), source);
        if (!name.empty()) {
            std::string qualified = name;
            if (!scope_stack.empty()) {
                qualified = scope_stack.back() + "." + name;
            }
            out.push_back(CodeUnit{file_path, qualified, node_text(node, source)});
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        walk_java(ts_node_child(node, i), source, file_path, scope_stack, out);
    }

    if (pushed_scope) scope_stack.pop_back();
}

// ---------------------------------------------------------------------------
// Generic AST dispatch
// ---------------------------------------------------------------------------
void walk_tree(LangId lang, TSNode root, const std::string& source,
               const std::string& file_path, std::vector<CodeUnit>& out) {
    std::vector<std::string> stack;
    switch (lang) {
        case LangId::Cpp:
            walk_cpp(root, source, file_path, stack, out);
            break;
        case LangId::Python:
            walk_python(root, source, file_path, stack, out);
            break;
        case LangId::JavaScript:
        case LangId::TypeScript:
            walk_js(root, source, file_path, stack, out);
            break;
        case LangId::Go:
            walk_go(root, source, file_path, out);
            break;
        case LangId::Rust:
            walk_rust(root, source, file_path, stack, out);
            break;
        case LangId::Java:
            walk_java(root, source, file_path, stack, out);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Universal text chunker (architecture review #3 bullet 2)
// Sliding-window chunker: ~40 lines per chunk, 10-line overlap, so every
// repository gets indexed regardless of programming language.
// ---------------------------------------------------------------------------
constexpr int kChunkLines = 40;
constexpr int kOverlapLines = 10;

std::vector<CodeUnit> text_chunk(const std::string& file_path, const std::string& source) {
    std::vector<CodeUnit> units;

    std::vector<std::string> lines;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    if (lines.empty()) return units;

    const int total = static_cast<int>(lines.size());
    const int step = kChunkLines - kOverlapLines;

    for (int start = 0; start < total; start += step) {
        const int end = std::min(start + kChunkLines, total);

        std::ostringstream content;
        for (int i = start; i < end; ++i) {
            content << lines[i] << "\n";
        }

        // Unit name: e.g. "README.md:L1-L40"
        const std::string basename = std::filesystem::path(file_path).filename().string();
        const std::string unit_name = basename + ":L" + std::to_string(start + 1) +
                                      "-L" + std::to_string(end);
        units.push_back(CodeUnit{file_path, unit_name, content.str()});

        if (end >= total) break;
    }
    return units;
}

}  // namespace

// ---------------------------------------------------------------------------
// is_cpp_source_file_impl (for backward-compatible inline alias in header)
// ---------------------------------------------------------------------------
bool is_cpp_source_file_impl(const std::string& file_path) {
    const std::string ext = get_extension(file_path);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
           ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

// ---------------------------------------------------------------------------
// Public API: file classification
// ---------------------------------------------------------------------------
bool is_parseable_file(const std::string& file_path) {
    return detect_language(get_extension(file_path)) != LangId::Unknown;
}

bool is_text_chunkable_file(const std::string& file_path) {
    if (is_parseable_file(file_path)) return false;  // handled by grammar
    const std::string ext = get_extension(file_path);
    if (ext.empty()) return false;  // extensionless files — skip
    return binary_extensions().count(ext) == 0;
}

// ---------------------------------------------------------------------------
// CodeParser Pimpl
// ---------------------------------------------------------------------------
struct CodeParser::Impl {
    TSParser* parser = nullptr;

    Impl() {
        parser = ts_parser_new();
        // Language is set dynamically per-file in parse_with_lang().
    }

    ~Impl() {
        if (parser) ts_parser_delete(parser);
    }

    std::vector<CodeUnit> parse_with_lang(LangId lang, const std::string& display_path,
                                           const std::string& source) {
        std::vector<CodeUnit> units;
        if (!parser || source.empty()) return units;

        const TSLanguage* ts_lang = get_language(lang);
        if (!ts_lang) return units;

        ts_parser_set_language(parser, ts_lang);

        TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(),
                                               static_cast<uint32_t>(source.size()));
        if (!tree) return units;

        TSNode root = ts_tree_root_node(tree);
        walk_tree(lang, root, source, display_path, units);

        ts_tree_delete(tree);
        return units;
    }
};

CodeParser::CodeParser() : impl_(std::make_unique<Impl>()) {}
CodeParser::~CodeParser() = default;

std::vector<CodeUnit> CodeParser::parse_source(const std::string& display_path,
                                                const std::string& source) const {
    if (!impl_ || source.empty()) return {};

    const std::string ext = get_extension(display_path);
    const LangId lang = detect_language(ext);

    if (lang != LangId::Unknown) {
        return impl_->parse_with_lang(lang, display_path, source);
    }

    // Fall back to text chunking for non-code files.
    if (is_text_chunkable_file(display_path)) {
        return text_chunk(display_path, source);
    }

    return {};
}

std::vector<CodeUnit> CodeParser::parse_file(const std::string& file_path) const {
    // Quick reject binary/unsupported files before reading from disk.
    if (!is_parseable_file(file_path) && !is_text_chunkable_file(file_path)) {
        return {};
    }

    std::ifstream f(file_path, std::ios::binary);
    if (!f) return {};

    std::ostringstream buf;
    buf << f.rdbuf();
    return parse_source(file_path, buf.str());
}

}  // namespace mygit::rag
