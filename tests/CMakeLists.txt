# Catch2 unit tests for the Sage server.

add_executable(sage-tests
    unit/test_jsonrpc.cpp
    unit/test_tool_registry.cpp
    unit/test_server.cpp
    unit/test_bridge_protocol.cpp
)
target_link_libraries(sage-tests
    PRIVATE sage-bridge
            Catch2::Catch2WithMain
            sage-warnings
            sage-sanitizers)
target_compile_features(sage-tests PRIVATE cxx_std_23)

set_target_properties(sage-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# CTest integration via Catch2 test discovery
catch_discover_tests(sage-tests)

# ---- Bridge integration smoke (standalone) ----
# Built but NOT registered with ctest — requires a running sage-server.
add_executable(sage-bridge-smoke
    integration/bridge_smoke.cpp
)
target_link_libraries(sage-bridge-smoke
    PRIVATE ixwebsocket::ixwebsocket
            nlohmann_json::nlohmann_json
            sage-warnings sage-sanitizers)
target_compile_features(sage-bridge-smoke PRIVATE cxx_std_23)
set_target_properties(sage-bridge-smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
