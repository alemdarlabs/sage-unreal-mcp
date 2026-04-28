#pragma once

#include "graph/graph_store.h"

#include <string>
#include <string_view>

namespace sage::graph {

// Phase 2.5 — read-only Cypher subset for the `query_graph` MCP tool.
// ADR-011 layer 2: a keyword whitelist + bound checks, NOT a full AST
// parser. Sufficient because:
//   - The graph layer is owner-trusted; this is a guardrail against the
//     LLM's exuberance, not adversarial bytes.
//   - Whitelist failures fall through to clear errors before any kuzu
//     work happens — fast feedback.
//
// Rules
//   - Reject any of: CREATE, DELETE, DETACH, SET, REMOVE, MERGE, DROP,
//     ALTER, COPY, LOAD, INSERT (case-insensitive, word-boundary).
//   - CALL is rejected to keep server-administrative procs out of the
//     agent-facing surface (use ingestSnapshot or schema migrations).
//   - Variable-length relationships `[*N..M]` must be bounded with
//     M ≤ kMaxVariableLengthHops (10). Unbounded `*`, `*N..`, `*..M` are
//     rejected.
//   - LIMIT must be present after validation; the wrapper appends one if
//     the user omitted it.
//
// Validation is independent of the kuzu version; the planner enforces
// further safety (statement boundary, etc.).

struct CypherValidation {
    bool        ok = false;
    std::string error;
};

inline constexpr int kDefaultRowLimit       = 200;
inline constexpr int kMaxRowLimit           = 1000;
inline constexpr int kMaxVariableLengthHops = 10;

// Validate `cypher`. On success, `ok==true`. On failure, `error` carries
// a human-readable reason for the agent.
[[nodiscard]] CypherValidation validateReadOnlySubset(std::string_view cypher);

// Returns `cypher` with `; LIMIT N` appended if it doesn't already
// contain a top-level LIMIT clause (case-insensitive search outside of
// string literals). Trailing semicolons are stripped first.
[[nodiscard]] std::string ensureLimit(std::string_view cypher, int limit);

}  // namespace sage::graph
