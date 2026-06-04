# Sage server: MCP protocol + transport + tools
#
# Targets:
#   sage-mcp        STATIC  protocol layer (envelope, registry, dispatcher)
#   sage-transport  STATIC  HTTP+SSE wrapping cpp-httplib
#   sage-tools      STATIC  built-in tools (ping for now)
#   sage-server     EXEC    main entry point

# ---- mcp library ----
add_library(sage-mcp STATIC
    src/mcp/tool_registry.cpp
    src/mcp/server.cpp
)
target_include_directories(sage-mcp
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-mcp PUBLIC cxx_std_23)
target_link_libraries(sage-mcp
    PUBLIC  nlohmann_json::nlohmann_json
            spdlog::spdlog
    PRIVATE sage-warnings sage-sanitizers)

# ---- transport library ----
add_library(sage-transport STATIC
    src/transport/http_sse_server.cpp
    src/transport/stdio_mcp.cpp
)
target_include_directories(sage-transport
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-transport PUBLIC cxx_std_23)
target_link_libraries(sage-transport
    PUBLIC  sage-mcp httplib::httplib
    PRIVATE sage-warnings sage-sanitizers)

# ---- tools library ----
add_library(sage-tools STATIC
    src/tools/builtin.cpp
    src/tools/phase4_schemas.cpp
    src/tools/source_intelligence_tools.cpp
)
target_include_directories(sage-tools
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-tools PUBLIC cxx_std_23)
target_link_libraries(sage-tools
    PUBLIC  sage-mcp
    PRIVATE sage-warnings sage-sanitizers)


# ---- bridge library (plugin↔server WebSocket, ADR-015) ----
add_library(sage-bridge STATIC
    src/bridge/protocol.cpp
    src/bridge/bridge_server.cpp
)
target_include_directories(sage-bridge
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-bridge PUBLIC cxx_std_23)
target_link_libraries(sage-bridge
    PUBLIC  sage-mcp ixwebsocket::ixwebsocket
    PRIVATE sage-warnings sage-sanitizers)

# ---- executable ----
add_executable(sage-server
    src/main.cpp
    src/tools/restart_orchestrator.cpp
    src/util/crash_handler.cpp
)
target_link_libraries(sage-server
    PRIVATE sage-transport sage-tools sage-bridge
            sage-warnings sage-sanitizers)
if(WIN32)
    # crash_handler.cpp uses MiniDumpWriteDump (dbghelp). #pragma comment
    # already pulls dbghelp.lib in MSVC; keep an explicit link for clarity
    # and so the dependency survives moves to clang-cl / lld-link.
    target_link_libraries(sage-server PRIVATE dbghelp)
endif()
set_target_properties(sage-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
