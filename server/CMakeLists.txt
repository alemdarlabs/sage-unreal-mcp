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
)
target_include_directories(sage-tools
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-tools PUBLIC cxx_std_23)
target_link_libraries(sage-tools
    PUBLIC  sage-mcp
    PRIVATE sage-warnings sage-sanitizers)

# ---- graph library (KuzuDB embedded; Phase 2 knowledge layer) ----
# Compiled as C++20 — kuzu.hpp's forward-declared ExtraTypeInfo trips libc++
# constexpr ~unique_ptr static_assert under C++23. The rest of Sage stays on
# C++23; only this TU needs the downgrade. See .claude/notes/lessons.md.
add_library(sage-graph STATIC
    src/graph/kuzu_graph_store.cpp
    src/graph/graph_schema.cpp
    src/graph/graph_store_manager.cpp
    src/graph/asset_indexer.cpp
    src/graph/cypher_subset.cpp
)
target_include_directories(sage-graph
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(sage-graph
    PUBLIC  nlohmann_json::nlohmann_json
            kuzu::kuzu
    PRIVATE spdlog::spdlog
            sage-warnings)
# Compiled as C++20: kuzu.hpp's forward-declared ExtraTypeInfo + default-arg
# unique_ptr trips libc++'s constexpr ~unique_ptr static_assert under C++23.
# GraphResult therefore uses std::variant (C++17) rather than std::expected
# (C++23) so the public header is version-neutral. The rest of Sage stays
# C++23. See .claude/notes/lessons.md.
set_target_properties(sage-graph PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF)

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
)
target_link_libraries(sage-server
    PRIVATE sage-transport sage-tools sage-bridge sage-graph
            sage-warnings sage-sanitizers)
set_target_properties(sage-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH   "${KUZU_DIR}"
    INSTALL_RPATH "@executable_path")
# sage-server transitively pulls libkuzu via sage-graph; copy the dylib next
# to the executable so loader finds it without DYLD_LIBRARY_PATH.
add_custom_command(TARGET sage-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KUZU_DIR}/${KUZU_LIB_NAME}"
        "$<TARGET_FILE_DIR:sage-server>/${KUZU_LIB_NAME}")
