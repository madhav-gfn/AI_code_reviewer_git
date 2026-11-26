#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "rag/code_parser.h"

using mygit::rag::CodeParser;
using mygit::rag::CodeUnit;
using mygit::rag::is_cpp_source_file;

namespace {

const CodeUnit* find_unit(const std::vector<CodeUnit>& units, const std::string& name) {
    auto it = std::find_if(units.begin(), units.end(),
                            [&](const CodeUnit& u) { return u.unit_name == name; });
    return it == units.end() ? nullptr : &*it;
}

constexpr const char* kFixture = R"cpp(
int add(int a, int b) {
    return a + b;
}

class DecisionEngine {
public:
    bool should_allow(int x) {
        return x > 0;
    }

    bool should_deny(int x);
};

bool DecisionEngine::should_deny(int x) {
    return x <= 0;
}

struct Config {
    int value;
};
)cpp";

}  // namespace

TEST_CASE("is_cpp_source_file recognizes common C/C++ extensions") {
    REQUIRE(is_cpp_source_file("foo.cpp"));
    REQUIRE(is_cpp_source_file("foo.h"));
    REQUIRE(is_cpp_source_file("foo.hpp"));
    REQUIRE(is_cpp_source_file("foo.cc"));
    REQUIRE_FALSE(is_cpp_source_file("foo.md"));
    REQUIRE_FALSE(is_cpp_source_file("foo.py"));
    REQUIRE_FALSE(is_cpp_source_file("foo"));
}

TEST_CASE("CodeParser extracts free functions, classes, inline methods, "
          "out-of-line methods, and structs") {
    const CodeParser parser;
    const std::vector<CodeUnit> units = parser.parse_source("fixture.cpp", kFixture);

    REQUIRE(units.size() == 5);
    for (const CodeUnit& u : units) {
        REQUIRE(u.file_path == "fixture.cpp");
    }

    const CodeUnit* add = find_unit(units, "add");
    REQUIRE(add != nullptr);
    REQUIRE(add->content.find("return a + b;") != std::string::npos);

    const CodeUnit* cls = find_unit(units, "DecisionEngine");
    REQUIRE(cls != nullptr);
    REQUIRE(cls->content.find("should_allow") != std::string::npos);
    REQUIRE(cls->content.find("should_deny") != std::string::npos);

    const CodeUnit* inline_method = find_unit(units, "DecisionEngine::should_allow");
    REQUIRE(inline_method != nullptr);
    REQUIRE(inline_method->content.find("return x > 0;") != std::string::npos);

    // Out-of-line definitions already carry "Class::member" via
    // qualified_identifier, so this must not come out double-prefixed as
    // "DecisionEngine::DecisionEngine::should_deny".
    const CodeUnit* out_of_line = find_unit(units, "DecisionEngine::should_deny");
    REQUIRE(out_of_line != nullptr);
    REQUIRE(out_of_line->content.find("return x <= 0;") != std::string::npos);

    const CodeUnit* strct = find_unit(units, "Config");
    REQUIRE(strct != nullptr);
    REQUIRE(strct->content.find("int value;") != std::string::npos);
}

TEST_CASE("CodeParser returns text chunks for non-code text files") {
    const CodeParser parser;

    // .md files are now text-chunked (architecture review #3 bullet 2),
    // so parse_source produces sliding-window chunks, not empty.
    const auto chunks = parser.parse_source("fixture.md", kFixture);
    REQUIRE_FALSE(chunks.empty());
    REQUIRE(chunks[0].file_path == "fixture.md");
    REQUIRE(chunks[0].unit_name.find("fixture.md:L") != std::string::npos);
}

TEST_CASE("CodeParser returns nothing for unreadable paths") {
    const CodeParser parser;
    REQUIRE(parser.parse_file("this/path/does/not/exist.cpp").empty());
}

TEST_CASE("CodeParser handles empty source without crashing") {
    const CodeParser parser;
    REQUIRE(parser.parse_source("empty.cpp", "").empty());
}

TEST_CASE("is_parseable_file recognizes multi-language extensions") {
    using mygit::rag::is_parseable_file;

    REQUIRE(is_parseable_file("main.cpp"));
    REQUIRE(is_parseable_file("main.py"));
    REQUIRE(is_parseable_file("index.js"));
    REQUIRE(is_parseable_file("app.ts"));
    REQUIRE(is_parseable_file("main.go"));
    REQUIRE(is_parseable_file("lib.rs"));
    REQUIRE(is_parseable_file("Main.java"));

    REQUIRE_FALSE(is_parseable_file("README.md"));
    REQUIRE_FALSE(is_parseable_file("data.yaml"));
    REQUIRE_FALSE(is_parseable_file("image.png"));
}

TEST_CASE("is_text_chunkable_file identifies text files without grammars") {
    using mygit::rag::is_text_chunkable_file;

    REQUIRE(is_text_chunkable_file("README.md"));
    REQUIRE(is_text_chunkable_file("config.yaml"));
    REQUIRE(is_text_chunkable_file("Makefile.txt"));

    // Grammar-supported files should NOT be text-chunked.
    REQUIRE_FALSE(is_text_chunkable_file("main.cpp"));
    REQUIRE_FALSE(is_text_chunkable_file("main.py"));

    // Binary files should NOT be text-chunked.
    REQUIRE_FALSE(is_text_chunkable_file("logo.png"));
    REQUIRE_FALSE(is_text_chunkable_file("app.exe"));
}

TEST_CASE("CodeParser extracts Python functions and classes") {
    const CodeParser parser;
    const char* source = R"py(
class MyClass:
    def method(self):
        return 42

def free_function(x):
    return x + 1
)py";

    const auto units = parser.parse_source("example.py", source);
    REQUIRE_FALSE(units.empty());

    const CodeUnit* cls = find_unit(units, "MyClass");
    REQUIRE(cls != nullptr);

    const CodeUnit* method = find_unit(units, "MyClass.method");
    REQUIRE(method != nullptr);

    const CodeUnit* free_fn = find_unit(units, "free_function");
    REQUIRE(free_fn != nullptr);
}

TEST_CASE("CodeParser extracts JavaScript functions and classes") {
    const CodeParser parser;
    const char* source = R"js(
class Greeter {
    greet(name) {
        return `Hello, ${name}`;
    }
}

function standalone() {
    return true;
}
)js";

    const auto units = parser.parse_source("example.js", source);
    REQUIRE_FALSE(units.empty());

    const CodeUnit* cls = find_unit(units, "Greeter");
    REQUIRE(cls != nullptr);

    const CodeUnit* method = find_unit(units, "Greeter.greet");
    REQUIRE(method != nullptr);

    const CodeUnit* fn = find_unit(units, "standalone");
    REQUIRE(fn != nullptr);
}

TEST_CASE("Text chunker produces overlapping windows") {
    const CodeParser parser;

    // Build a source with 60 lines to trigger at least 2 chunks.
    std::string source;
    for (int i = 1; i <= 60; ++i) {
        source += "line " + std::to_string(i) + "\n";
    }

    const auto units = parser.parse_source("big.txt", source);
    REQUIRE(units.size() >= 2);
    REQUIRE(units[0].unit_name == "big.txt:L1-L40");
}
