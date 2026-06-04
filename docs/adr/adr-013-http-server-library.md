# ADR-013: HTTP Server Library - cpp-httplib

**Date:** 2026-04-27
**Status:** Accepted

## Context

ADR-001 selected a C++23 server but left the concrete HTTP server library open.
The MCP transport needed:

- `POST /mcp` for JSON-RPC requests and streamable responses.
- `GET /healthz` for health checks.
- Low-friction multi-client support.
- A clean separation from the plugin WebSocket bridge.

The server does not need a full web framework.

## Candidates

| Library | License | Header-only | SSE support | WebSocket | vcpkg | Transitive cost |
|---|---|---|---|---|---|---|
| cpp-httplib | MIT | Yes | Chunked provider | No | Yes | Optional OpenSSL/zlib |
| Crow | BSD-3 | No | Manual chunking | Yes | Yes | Boost.Asio, Boost.System |
| Boost.Beast | Boost | No | Low-level | Yes | Yes | Boost stack |
| Custom implementation | N/A | N/A | N/A | N/A | N/A | High maintenance |

## Decision

Use `cpp-httplib`.

## Rationale

1. The use case is a small HTTP surface, not a general web app.
2. The plugin bridge already uses a separate WebSocket transport.
3. `cpp-httplib` supports chunked responses, which maps well to streamable MCP
   responses.
4. Header-only integration keeps build footprint low.
5. The callback API keeps transport separate from MCP protocol handling.
6. vcpkg support is straightforward.
7. The library works across Windows, macOS, and Linux.

## Rejected Alternatives

- **Crow**: Built-in WebSocket support is redundant for Sage and the Boost
  dependency chain is heavier than needed.
- **Boost.Beast**: Powerful but too low-level and heavy for the target profile.
- **Custom HTTP**: Reimplementing HTTP/1.1 and SSE framing would add avoidable
  protocol and security risk.

## Consequences

Positive:

- Small transport implementation.
- Minimal dependency tree.
- No extra library is needed for streamable response support.
- Cross-platform behavior is simple.

Negative:

- TLS support requires enabling OpenSSL when needed.
- HTTP/2 is not available.
- Thread-per-connection behavior is not designed for high public-web
  concurrency, which is acceptable for Sage's local MCP profile.

## Affected Files

- `docs/engineering/tech-stack.md`
- `vcpkg.json`
- `server/src/transport/http_sse_server.{h,cpp}`
- `server/CMakeLists.txt`
