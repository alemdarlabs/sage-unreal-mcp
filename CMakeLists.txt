# Sage Unreal MCP — workspace root
#
# Layout:
#   server/   C++23 MCP server (executable + libraries)
#   tests/    Catch2 test suite
#   plugin/   Unreal plugin (UE BuildTool, NOT CMake)
#
# Usage:
#   cmake --preset debug
#   cmake --build --preset debug
#   ctest --preset debug

cmake_minimum_required(VERSION 3.25)

project(sage-unreal-mcp
    VERSION 0.1.7
    DESCRIPTION "Sage MCP server for Unreal Engine"
    HOMEPAGE_URL "https://github.com/alemdarlabs/sage-unreal-mcp"
    LANGUAGES CXX
)

# ---- Standard ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- MSVC CRT: force /MD even in Debug --------------------------------------
# Keep one CRT model across Sage and imported binary dependencies. This avoids
# Debug/Release CRT boundary mismatches in Windows builds while preserving the
# same runtime model used by packaged distribution binaries.
# CMP0091 (CMake >= 3.15) makes this a single set() instead of patching every
# target's compile flags.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:>DLL")
    # Even in Debug, link the *release* variant of every imported target
    # (vcpkg's spdlog/ixwebsocket/etc.). Without this, vcpkg's debug
    # binaries are /MDd while Sage itself is /MD, re-introducing the
    # LNK2038 _ITERATOR_DEBUG_LEVEL / RuntimeLibrary mismatch one DLL
    # boundary further down the stack.
    set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;RelWithDebInfo;")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# ---- Options ----------------------------------------------------------------
option(SAGE_SANITIZE             "AddressSanitizer + UndefinedBehaviorSanitizer (Debug)" ON)
option(SAGE_THREAD_SANITIZE      "ThreadSanitizer (mutually exclusive with SAGE_SANITIZE)" OFF)
option(SAGE_BUILD_TESTS          "Build Catch2 test suite" ON)
option(SAGE_WARNINGS_AS_ERRORS   "Promote warnings to errors" ON)

if(SAGE_SANITIZE AND SAGE_THREAD_SANITIZE)
    message(FATAL_ERROR "SAGE_SANITIZE and SAGE_THREAD_SANITIZE cannot be enabled at the same time")
endif()

# ---- Interface targets: warnings & sanitizers -------------------------------
add_library(sage-version INTERFACE)
target_compile_definitions(sage-version INTERFACE
    SAGE_SERVER_VERSION="${PROJECT_VERSION}"
    SAGE_PROTOCOL_VERSION="0.1.0")

add_library(sage-warnings INTERFACE)
if(MSVC)
    target_compile_options(sage-warnings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    # MSVC promotes POSIX/CRT names like getenv, strncpy, sprintf to C4996
    # "deprecated"; we treat warnings as errors so suppress the noise here
    # rather than wrapping every call site. Same convention CMake's own
    # internal projects use.
    target_compile_definitions(sage-warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    if(SAGE_WARNINGS_AS_ERRORS)
        target_compile_options(sage-warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(sage-warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion
        -Wnull-dereference -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough)
    if(SAGE_WARNINGS_AS_ERRORS)
        target_compile_options(sage-warnings INTERFACE -Werror)
    endif()
endif()

add_library(sage-sanitizers INTERFACE)
if(SAGE_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(sage-sanitizers INTERFACE
        -fsanitize=address -fsanitize=undefined
        -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(sage-sanitizers INTERFACE
        -fsanitize=address -fsanitize=undefined)
elseif(SAGE_THREAD_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MSVC)
    target_compile_options(sage-sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(sage-sanitizers INTERFACE
        -fsanitize=thread)
endif()

# ---- Dependencies (vcpkg manifest) ------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog        CONFIG REQUIRED)
find_package(httplib       CONFIG REQUIRED)
find_package(ixwebsocket   CONFIG REQUIRED)

# vcpkg's ixwebsocket port doesn't propagate Windows system libraries
# pulled in by mbedcrypto's entropy_poll (BCryptGenRandom -> bcrypt.lib).
# Patch the imported target so every consumer links bcrypt transparently.
if(WIN32 AND TARGET ixwebsocket::ixwebsocket)
    set_property(TARGET ixwebsocket::ixwebsocket APPEND PROPERTY
                 INTERFACE_LINK_LIBRARIES bcrypt)
endif()

# ---- Subdirectories ---------------------------------------------------------
add_subdirectory(server)

if(SAGE_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
    add_subdirectory(tests)
endif()

# ---- Summary ----------------------------------------------------------------
message(STATUS "")
message(STATUS "==== Sage build configuration ====")
message(STATUS "  Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard         : ${CMAKE_CXX_STANDARD}")
message(STATUS "  ASan + UBSan         : ${SAGE_SANITIZE}")
message(STATUS "  TSan                 : ${SAGE_THREAD_SANITIZE}")
message(STATUS "  Tests                : ${SAGE_BUILD_TESTS}")
message(STATUS "  Warnings as errors   : ${SAGE_WARNINGS_AS_ERRORS}")
message(STATUS "==================================")
message(STATUS "")
