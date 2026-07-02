cmake_minimum_required(VERSION 3.21)
project(mygit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# --- Core library --------------------------------------------------------
add_library(mygit_core
    src/types.cpp
    commands/push_command.cpp
    commands/review_command.cpp
    commands/commit_command.cpp
    commands/install_command.cpp
    commands/setup_command.cpp
    config/mygit_config.cpp
    git/git_diff.cpp
    git/git_status.cpp
    git/git_runner.cpp
    ai/llama_client.cpp
    ai/prompt_builder.cpp
    parsers/json_parser.cpp
    decision_engine/decision_engine.cpp
    reports/report_generator.cpp
    database/sqlite_manager.cpp
    ui/terminal_ui.cpp
)

target_include_directories(mygit_core PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(mygit_core PUBLIC
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    SQLite::SQLite3
    llama
    ftxui::dom
    ftxui::screen
)

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
    add_executable(mygit_tests tests/test_decision_engine.cpp)
    target_link_libraries(mygit_tests PRIVATE mygit_core Catch2::Catch2WithMain)

    enable_testing()
    add_test(NAME mygit_tests COMMAND mygit_tests)
endif()
