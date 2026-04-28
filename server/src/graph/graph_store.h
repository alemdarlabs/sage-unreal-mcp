#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace sage::graph {

using Json = nlohmann::json;

// Error returned by graph operations. `code` mirrors api-spec.md error codes
// when applicable (Sage range -32xxx); 0 = generic, remapped at the MCP
// boundary.
struct GraphError {
    std::string message;
    int         code = 0;
};

// Result envelope. Successful: a Json `{rows, schema, row_count}`; failure:
// GraphError. We use std::variant rather than std::expected because the
// kuzu prebuilt forces this TU to compile as C++20 (kuzu.hpp's
// forward-declared ExtraTypeInfo trips libc++'s constexpr unique_ptr
// destructor under C++23). std::variant is C++17 and thus version-neutral.
using GraphResult = std::variant<Json, GraphError>;

inline bool is_ok(const GraphResult& r)    { return std::holds_alternative<Json>(r); }
inline bool is_error(const GraphResult& r) { return std::holds_alternative<GraphError>(r); }

inline const Json&        value_of(const GraphResult& r) { return std::get<Json>(r); }
inline const GraphError&  error_of(const GraphResult& r) { return std::get<GraphError>(r); }

// Abstract graph store. Implementations:
//   - KuzuGraphStore : production (kuzu v0.11 embedded)
//   - MockGraphStore : test fixture (Phase 2.1d)
//
// Cypher dialect; subset enforcement (ADR-011 layer 2 whitelist) is applied
// at the MCP tool boundary, not here. This layer is raw access — owners are
// trusted server code.
class GraphStore {
public:
    virtual ~GraphStore() = default;

    GraphStore() = default;
    GraphStore(const GraphStore&)            = delete;
    GraphStore& operator=(const GraphStore&) = delete;
    GraphStore(GraphStore&&)                 = delete;
    GraphStore& operator=(GraphStore&&)      = delete;

    // Execute a Cypher statement (DDL, DML, or read query).
    [[nodiscard]] virtual GraphResult execute(std::string_view cypher) = 0;

    // Reserved for prepared-statement / parameter binding (Phase 2.1c).
    [[nodiscard]] virtual GraphResult execute(std::string_view cypher,
                                               const Json& /*params*/) {
        return execute(cypher);
    }

    [[nodiscard]] virtual bool isOpen() const = 0;
};

}  // namespace sage::graph
