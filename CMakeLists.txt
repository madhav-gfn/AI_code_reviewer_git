cmake_minimum_required(VERSION 3.21)

# Keep the user-facing build command at the repository root while the client
# implementation itself lives in client/. vcpkg manifest dependencies are
# declared alongside that client build.
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/client" CACHE PATH "vcpkg manifest directory")
project(mygit VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(client)
