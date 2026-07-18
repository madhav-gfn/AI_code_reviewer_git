#include "rag/code_parser.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <tree_sitter/api.h>

namespace mygit::rag {

// Declared by the tree-sitter-cpp grammar (compiled as a separate static lib
// via CMake FetchContent - see CMakeLists.txt). Not exposed through any
// vcpkg-installed header, so we declare the C symbol ourselves.
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace {

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
void walk(TSNode node, const std::string& source, const std::string& file_path,
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
        walk(ts_node_child(node, i), source, file_path, class_stack, out);
    }

    if (pushed_class) class_stack.pop_back();
}

}  // namespace

bool is_cpp_source_file(const std::string& file_path) {
    static const char* kExtensions[] = {".c", ".cc", ".cpp", ".cxx",
                                         ".h", ".hh", ".hpp", ".hxx"};
    const auto dot = file_path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = file_path.substr(dot);
    for (const char* candidate : kExtensions) {
        if (ext == candidate) return true;
    }
    return false;
}

struct CodeParser::Impl {
    TSParser* parser = nullptr;

    Impl() {
        parser = ts_parser_new();
        if (parser) {
            ts_parser_set_language(parser, tree_sitter_cpp());
        }
    }

    ~Impl() {
        if (parser) ts_parser_delete(parser);
    }
};

CodeParser::CodeParser() : impl_(std::make_unique<Impl>()) {}
CodeParser::~CodeParser() = default;

std::vector<CodeUnit> CodeParser::parse_source(const std::string& display_path,
                                                const std::string& source) const {
    std::vector<CodeUnit> units;
    if (!impl_ || !impl_->parser) return units;
    if (!is_cpp_source_file(display_path)) return units;
    if (source.empty()) return units;

    TSTree* tree = ts_parser_parse_string(impl_->parser, nullptr, source.c_str(),
                                           static_cast<uint32_t>(source.size()));
    if (!tree) return units;

    TSNode root = ts_tree_root_node(tree);
    std::vector<std::string> class_stack;
    walk(root, source, display_path, class_stack, units);

    ts_tree_delete(tree);
    return units;
}

std::vector<CodeUnit> CodeParser::parse_file(const std::string& file_path) const {
    if (!is_cpp_source_file(file_path)) return {};

    std::ifstream f(file_path, std::ios::binary);
    if (!f) return {};

    std::ostringstream buf;
    buf << f.rdbuf();
    return parse_source(file_path, buf.str());
}

}  // namespace mygit::rag
