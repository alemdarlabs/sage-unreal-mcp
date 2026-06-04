# ADR-015: Plugin Bridge WebSocket Library - ixwebsocket

**Date:** 2026-04-28
**Status:** Accepted
**Supersedes:** ADR-001 plugin bridge library choice

## Context

ADR-001 initially favored uWebSockets for the server-to-plugin bridge. During
bridge implementation, the team re-evaluated the choice against Sage's actual
profile:

- Localhost-only traffic.
- Request/response tool dispatch.
- Low client count.
- Cross-platform packaging.
- Simple CMake/vcpkg integration.

## Decision

Use `ixwebsocket` for the plugin bridge.

## Rationale

1. Sage does not need a high-throughput public WebSocket server.
2. The request/response model maps cleanly to promises/futures.
3. Build and packaging integration is simpler than uWebSockets.
4. vcpkg support is straightforward.
5. The choice matches the right-size dependency discipline used by ADR-013.

## Consequences

Positive:

- Lower integration risk.
- Simpler Windows/macOS/Linux packaging.
- Cleaner synchronous request/response bridge code.

Negative:

- Lower theoretical throughput than uWebSockets.
- The bridge still needs careful lifecycle and close-handling code.
- The server owns the bridge thread lifecycle.

## Implementation Notes

The bridge starts before the HTTP listener accepts MCP traffic and stops during
server shutdown. Tool dispatch remains request/response oriented; event streams
can be layered separately if needed.
