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
)
target_include_directories(sage-tools
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features(sage-tools PUBLIC cxx_std_23)
target_link_libraries(sage-tools
    PUBLIC  sage-mcp
    PRIVATE sage-warnings sage-sanitizers)

# ---- executable ----
add_executable(sage-server src/main.cpp)
target_link_libraries(sage-server
    PRIVATE sage-transport sage-tools
            sage-warnings sage-sanitizers)
set_target_properties(sage-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
