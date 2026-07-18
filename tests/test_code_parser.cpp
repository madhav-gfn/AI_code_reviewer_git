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

TEST_CASE("CodeParser returns nothing for non-C++ files or unreadable paths") {
    const CodeParser parser;

    REQUIRE(parser.parse_source("fixture.md", kFixture).empty());
    REQUIRE(parser.parse_file("this/path/does/not/exist.cpp").empty());
}

TEST_CASE("CodeParser handles empty source without crashing") {
    const CodeParser parser;
    REQUIRE(parser.parse_source("empty.cpp", "").empty());
}
