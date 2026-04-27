# Catch2 unit tests for the Sage server.

add_executable(sage-tests
    unit/test_jsonrpc.cpp
    unit/test_tool_registry.cpp
    unit/test_server.cpp
)
target_link_libraries(sage-tests
    PRIVATE sage-mcp
            Catch2::Catch2WithMain
            sage-warnings
            sage-sanitizers)
target_compile_features(sage-tests PRIVATE cxx_std_23)

set_target_properties(sage-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# CTest integration via Catch2 test discovery
catch_discover_tests(sage-tests)
