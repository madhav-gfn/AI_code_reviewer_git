cmake_minimum_required(VERSION 3.21)
project(mygit VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- Dependencies (V1) -------------------------------------------------
# libgit2 is intentionally not required yet:
#   - FR-2 ships with shell-command git access first, libgit2 is V2.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(llama CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(httplib CONFIG REQUIRED)

# RAG pipeline (bottleneck #3 - repository-aware context retrieval).
# onnxruntime is CPU-only for now: its `cuda` feature additionally needs
# cuDNN, which vcpkg cannot fetch itself (NVIDIA gates it behind a manual,
# authenticated download - see rag/embedder.cpp for how to flip this on
# once cuDNN is installed locally).
find_package(onnxruntime CONFIG REQUIRED)
find_package(faiss CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)

# tree-sitter-cpp isn't in vcpkg (only the tree-sitter runtime and the C
# grammar are), so the C++ grammar is pulled straight from upstream and
# compiled as a small static library. Deliberately NOT using
# FetchContent_MakeAvailable(): the grammar's own CMakeLists.txt tries to
# *regenerate* parser.c via the tree-sitter CLI (not installed here) - we
# don't need that, parser.c/scanner.c ship pre-generated in the repo. Also
# NOT using FetchContent_Populate(): its git-subbuild mechanism is
# deprecated as of CMake 3.30 and hit an unrelated "Permission denied"
# recompaction failure in testing here. A plain download+extract of the
# release tarball sidesteps both issues.
set(TREE_SITTER_CPP_SRC_DIR "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp-src")
if(NOT EXISTS "${TREE_SITTER_CPP_SRC_DIR}/src/parser.c")
    file(DOWNLOAD
        "https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz"
        "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp.tar.gz"
    )
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp-extracted")
    file(ARCHIVE_EXTRACT
        INPUT "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp.tar.gz"
        DESTINATION "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp-extracted"
    )
    # GitHub's tarball extracts to a single "tree-sitter-cpp-<version>"
    # subdirectory - find it without hardcoding the exact name.
    file(GLOB TREE_SITTER_CPP_EXTRACTED_DIRS "${CMAKE_BINARY_DIR}/_deps/tree-sitter-cpp-extracted/*")
    list(GET TREE_SITTER_CPP_EXTRACTED_DIRS 0 TREE_SITTER_CPP_EXTRACTED_DIR)
    file(RENAME "${TREE_SITTER_CPP_EXTRACTED_DIR}" "${TREE_SITTER_CPP_SRC_DIR}")
endif()

add_library(tree_sitter_cpp_grammar STATIC
    ${TREE_SITTER_CPP_SRC_DIR}/src/parser.c
    ${TREE_SITTER_CPP_SRC_DIR}/src/scanner.c
)
target_include_directories(tree_sitter_cpp_grammar PRIVATE
    ${TREE_SITTER_CPP_SRC_DIR}/src
)
set_target_properties(tree_sitter_cpp_grammar PROPERTIES LINKER_LANGUAGE C)

# --- Multi-language Tree-sitter grammars (architecture review #3) -----------
# Same tarball-extract pattern as tree-sitter-cpp above: download + extract +
# build a tiny static lib, no FetchContent_MakeAvailable (grammar CMakeLists
# try to regenerate parser.c via the tree-sitter CLI which isn't installed).
#
# Helper macro: download a grammar tarball, extract it, and build a static lib.
macro(add_tree_sitter_grammar NAME VERSION)
    set(_TS_${NAME}_SRC_DIR "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}-src")
    if(NOT EXISTS "${_TS_${NAME}_SRC_DIR}/src/parser.c")
        file(DOWNLOAD
            "https://github.com/tree-sitter/tree-sitter-${NAME}/archive/refs/tags/v${VERSION}.tar.gz"
            "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}.tar.gz"
        )
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}-extracted")
        file(ARCHIVE_EXTRACT
            INPUT "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}.tar.gz"
            DESTINATION "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}-extracted"
        )
        file(GLOB _TS_${NAME}_DIRS "${CMAKE_BINARY_DIR}/_deps/tree-sitter-${NAME}-extracted/*")
        list(GET _TS_${NAME}_DIRS 0 _TS_${NAME}_EXTRACTED_DIR)
        file(RENAME "${_TS_${NAME}_EXTRACTED_DIR}" "${_TS_${NAME}_SRC_DIR}")
    endif()

    # Build the grammar's parser.c (and scanner.c if it exists) as a static lib.
    set(_TS_${NAME}_SOURCES "${_TS_${NAME}_SRC_DIR}/src/parser.c")
    if(EXISTS "${_TS_${NAME}_SRC_DIR}/src/scanner.c")
        list(APPEND _TS_${NAME}_SOURCES "${_TS_${NAME}_SRC_DIR}/src/scanner.c")
    endif()

    add_library(tree_sitter_${NAME}_grammar STATIC ${_TS_${NAME}_SOURCES})
    target_include_directories(tree_sitter_${NAME}_grammar PRIVATE
        ${_TS_${NAME}_SRC_DIR}/src
    )
    set_target_properties(tree_sitter_${NAME}_grammar PROPERTIES LINKER_LANGUAGE C)
endmacro()

add_tree_sitter_grammar(python      0.23.6)
add_tree_sitter_grammar(javascript  0.23.1)
add_tree_sitter_grammar(go          0.23.4)
add_tree_sitter_grammar(rust        0.23.2)
add_tree_sitter_grammar(java        0.23.5)

# TypeScript is special: its repo contains two grammars (typescript + tsx)
# under separate subdirectories, each with their own parser.c/scanner.c.
set(_TS_typescript_SRC_DIR "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript-src")
if(NOT EXISTS "${_TS_typescript_SRC_DIR}/typescript/src/parser.c")
    file(DOWNLOAD
        "https://github.com/tree-sitter/tree-sitter-typescript/archive/refs/tags/v0.23.2.tar.gz"
        "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript.tar.gz"
    )
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript-extracted")
    file(ARCHIVE_EXTRACT
        INPUT "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript.tar.gz"
        DESTINATION "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript-extracted"
    )
    file(GLOB _TS_typescript_DIRS "${CMAKE_BINARY_DIR}/_deps/tree-sitter-typescript-extracted/*")
    list(GET _TS_typescript_DIRS 0 _TS_typescript_EXTRACTED_DIR)
    file(RENAME "${_TS_typescript_EXTRACTED_DIR}" "${_TS_typescript_SRC_DIR}")
endif()

set(_TS_TS_SOURCES "${_TS_typescript_SRC_DIR}/typescript/src/parser.c")
if(EXISTS "${_TS_typescript_SRC_DIR}/typescript/src/scanner.c")
    list(APPEND _TS_TS_SOURCES "${_TS_typescript_SRC_DIR}/typescript/src/scanner.c")
endif()
add_library(tree_sitter_typescript_grammar STATIC ${_TS_TS_SOURCES})
target_include_directories(tree_sitter_typescript_grammar PRIVATE
    ${_TS_typescript_SRC_DIR}/typescript/src
)
set_target_properties(tree_sitter_typescript_grammar PROPERTIES LINKER_LANGUAGE C)


# --- Core library --------------------------------------------------------
# Explicitly STATIC: without this, mygit_core follows BUILD_SHARED_LIBS,
# which the x64-windows-release vcpkg triplet's dynamic linkage default
# leaked into (making it a DLL) - and since nothing here is annotated with
# __declspec(dllexport), that silently exported zero symbols, breaking both
# mygit.exe and mygit_tests.exe with "unresolved external" for everything,
# not just the new RAG code.
add_library(mygit_core STATIC
    src/types.cpp
    commands/push_command.cpp
    commands/review_command.cpp
    commands/commit_command.cpp
    commands/history_command.cpp
    commands/install_command.cpp
    commands/setup_command.cpp
    commands/daemon_command.cpp
    commands/init_command.cpp
    config/mygit_config.cpp
    diff_filter/diff_filter.cpp
    git/git_diff.cpp
    git/git_status.cpp
    git/git_runner.cpp
    ai/llama_client.cpp
    ai/prompt_builder.cpp
    ai/review_aggregator.cpp
    daemon/daemon_server.cpp
    daemon/daemon_client.cpp
    parsers/json_parser.cpp
    decision_engine/decision_engine.cpp
    reports/report_generator.cpp
    database/sqlite_manager.cpp
    database/async_db_writer.cpp
    logger/review_logger.cpp
    ui/terminal_ui.cpp
    rag/code_parser.cpp
    rag/bpe_tokenizer.cpp
    rag/embedder.cpp
    rag/vector_store.cpp
    rag/rag_orchestrator.cpp
)

target_include_directories(mygit_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(mygit_core PUBLIC
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    SQLite3::SQLite3
    llama
    ftxui::dom
    ftxui::screen
    libgit2::libgit2package
    Threads::Threads
    httplib::httplib
    onnxruntime::onnxruntime
    faiss
    unofficial::tree-sitter::tree-sitter
    tree_sitter_cpp_grammar
    tree_sitter_python_grammar
    tree_sitter_javascript_grammar
    tree_sitter_typescript_grammar
    tree_sitter_go_grammar
    tree_sitter_rust_grammar
    tree_sitter_java_grammar
)

# Toggle for the ONNX Runtime CUDA execution provider (rag/embedder.cpp).
# Off by default because it needs both the vcpkg 'onnx-cuda' manifest
# feature *and* cuDNN installed locally (vcpkg can't fetch cuDNN itself -
# see vcpkg.json). Without this, Embedder always uses the CPU provider.
option(MYGIT_ENABLE_ONNX_CUDA "Enable the ONNX Runtime CUDA execution provider for embeddings" OFF)
if(MYGIT_ENABLE_ONNX_CUDA)
    target_compile_definitions(mygit_core PRIVATE MYGIT_ONNXRUNTIME_CUDA)
endif()

target_compile_options(mygit_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

# --- Executable ------------------------------------------------------------
add_executable(mygit src/main.cpp)
target_link_libraries(mygit PRIVATE mygit_core)

# --- Tests -------------------------------------------------------------
option(MYGIT_BUILD_TESTS "Build unit tests" ON)
if(MYGIT_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    add_executable(mygit_tests
        tests/test_decision_engine.cpp
        tests/test_diff_filter.cpp
        tests/test_code_parser.cpp
    )
    target_link_libraries(mygit_tests PRIVATE mygit_core Catch2::Catch2WithMain)

    enable_testing()
    add_test(NAME mygit_tests COMMAND mygit_tests)
endif()
