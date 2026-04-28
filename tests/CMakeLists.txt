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

# ---- Bridge integration harnesses (standalone) ----
# Built but NOT registered with ctest — they require a running sage-server.

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

add_executable(sage-bridge-mock-plugin
    integration/bridge_mock_plugin.cpp
)
target_link_libraries(sage-bridge-mock-plugin
    PRIVATE ixwebsocket::ixwebsocket
            nlohmann_json::nlohmann_json
            sage-warnings sage-sanitizers)
target_compile_features(sage-bridge-mock-plugin PRIVATE cxx_std_23)
set_target_properties(sage-bridge-mock-plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Kuzu smoke (Phase 2 baseline) --------------------------------------
# Kuzu's bundled header forward-declares `kuzu::common::ExtraTypeInfo` and
# uses it through `std::unique_ptr` in default arguments. Under C++23 with
# libc++, `~unique_ptr()` becomes constexpr, which triggers the
# `cannot delete an incomplete type` static_assert. Compile this target as
# C++20 (kuzu is built that way upstream) — the rest of Sage stays C++23.
# Sanitizers are also intentionally omitted (kuzu's prebuilt is not ASan-
# instrumented; ODR violations would surface at link time).
add_executable(sage-kuzu-smoke
    integration/kuzu_smoke.cpp
)
target_link_libraries(sage-kuzu-smoke
    PRIVATE kuzu::kuzu)
set_target_properties(sage-kuzu-smoke PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH   "${KUZU_DIR}"
    INSTALL_RPATH "@executable_path")
# Copy the kuzu shared lib next to the executable so default loader paths
# work without setting DYLD_LIBRARY_PATH at runtime.
add_custom_command(TARGET sage-kuzu-smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KUZU_DIR}/${KUZU_LIB_NAME}"
        "$<TARGET_FILE_DIR:sage-kuzu-smoke>/${KUZU_LIB_NAME}")

# ---- GraphStore abstraction smoke ----
# Driver doesn't include kuzu.hpp directly (sage-graph forwards-declares the
# kuzu types in its public header), so this TU stays C++23. No sanitizers —
# transitively links libkuzu (not ASan-instrumented).
add_executable(sage-graph-smoke
    integration/graph_store_smoke.cpp
)
target_link_libraries(sage-graph-smoke
    PRIVATE sage-graph)
target_compile_features(sage-graph-smoke PRIVATE cxx_std_23)
set_target_properties(sage-graph-smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH   "${KUZU_DIR}"
    INSTALL_RPATH "@executable_path")
add_custom_command(TARGET sage-graph-smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KUZU_DIR}/${KUZU_LIB_NAME}"
        "$<TARGET_FILE_DIR:sage-graph-smoke>/${KUZU_LIB_NAME}")

# ---- GraphStoreManager smoke ----
# Verifies slot-scoped DB acquisition, schema migration, and isolation.
add_executable(sage-graph-manager-smoke
    integration/graph_store_manager_smoke.cpp
)
target_link_libraries(sage-graph-manager-smoke
    PRIVATE sage-graph)
target_compile_features(sage-graph-manager-smoke PRIVATE cxx_std_23)
set_target_properties(sage-graph-manager-smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH   "${KUZU_DIR}"
    INSTALL_RPATH "@executable_path")
add_custom_command(TARGET sage-graph-manager-smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KUZU_DIR}/${KUZU_LIB_NAME}"
        "$<TARGET_FILE_DIR:sage-graph-manager-smoke>/${KUZU_LIB_NAME}")

# ---- AssetIndexer smoke ----
# Drives ingestAssets / getIndexStatus on a slot — full-replace semantics,
# Cypher escaping, validation. No plugin needed (mock asset list inline).
add_executable(sage-asset-indexer-smoke
    integration/asset_indexer_smoke.cpp
)
target_link_libraries(sage-asset-indexer-smoke
    PRIVATE sage-graph)
target_compile_features(sage-asset-indexer-smoke PRIVATE cxx_std_23)
set_target_properties(sage-asset-indexer-smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH   "${KUZU_DIR}"
    INSTALL_RPATH "@executable_path")
add_custom_command(TARGET sage-asset-indexer-smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KUZU_DIR}/${KUZU_LIB_NAME}"
        "$<TARGET_FILE_DIR:sage-asset-indexer-smoke>/${KUZU_LIB_NAME}")
