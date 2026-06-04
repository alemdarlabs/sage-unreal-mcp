# ADR-001: Technology Stack

**Date:** 2026-04-27
**Status:** Accepted
**Updated by:** ADR-013, ADR-015, ADR-018

## Context

Sage Unreal MCP needed a native stack that could work inside Unreal Editor,
serve MCP clients, package cleanly for studios, and keep latency low for large
Unreal projects.

The main constraints were:

- The plugin must use Unreal C++ to access reflection, AssetRegistry,
  transactions, editor subsystems, and runtime/editor APIs.
- The server must be a persistent process so editor restarts do not destroy the
  MCP session.
- The client transport and the plugin bridge have different lifecycle and
  protocol needs.
- Distribution should be practical for game teams and AI development tools.
- The architecture must leave room for large project understanding features.

## Decision

### Plugin Language

Use Unreal C++ in UPlugin format.

Rationale: Unreal C++ gives direct access to UCLASS, UPROPERTY, UFUNCTION,
AssetRegistry, FScopedTransaction, Slate, Blueprint graph APIs, and editor
subsystems. Python wrappers are incomplete for this surface and are not suitable
as the primary production plugin layer.

### Server Language

Use C++23 for the native server.

Rationale: The team already needs C++ for Unreal. Keeping the server in C++
reduces cross-language boundaries, supports native packaging, and gives the
highest performance ceiling. TypeScript, Rust, and Python were rejected for this
phase because they would add distribution friction, FFI seams, or editor/runtime
limitations.

### MCP Client Transport

Use HTTP + SSE for streamable MCP transport.

Rationale: stdio ties server lifetime to client lifetime. Sage needs a
persistent server that can survive editor restarts and support multiple clients.
HTTP + SSE gives simple request/response behavior plus progress streaming.

ADR-013 later selected `cpp-httplib` as the concrete HTTP server library.

### Plugin Bridge

Use a localhost WebSocket bridge between server and Unreal Editor.

Rationale: WebSocket is cross-platform, easy to inspect, and acceptable for
localhost overhead. It keeps the editor bridge independent from the MCP client
transport.

ADR-015 later selected `ixwebsocket` as the concrete implementation.

### Build System

Use CMake with vcpkg manifest mode.

Rationale: CMake is the standard native build system for C++ tooling and works
well with IDEs and CI. vcpkg manifest mode keeps dependencies reproducible.

### JSON

Use `nlohmann/json` for ergonomic JSON construction and simdjson for hot-path
parsing where needed.

### Logging

Use `spdlog`.

### Tests

Use Catch2 plus sanitizers where the platform supports them.

## Consequences

Positive:

- One native language across plugin and server.
- Direct Unreal API access.
- High performance ceiling.
- Clean single-binary server packaging.
- Clear transport separation between MCP clients and Unreal Editor.

Negative:

- MCP protocol implementation is manual.
- C++ memory and lifetime bugs require sanitizer discipline and careful review.
- Initial development velocity is lower than a scripting-first stack.

## Notes

The original graph-oriented storage assumptions were superseded by ADR-018,
which removed KuzuDB from the active runtime. This ADR remains the active stack
decision for the native server, plugin, build system, transport split, JSON,
logging, and test framework.
