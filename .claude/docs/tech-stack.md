# Tech Stack

Per-layer technology choices and rationale. See [ADR-001](../decisions/adr-001-tech-stack.md) for the canonical decision record.

## Plugin

| Component | Choice | Notes |
|---|---|---|
| Language | Unreal C++ | UPlugin format; mandatory (engine reflection access) |
| API surface | UE 5.x core | UCLASS/UPROPERTY/UFUNCTION reflection, AssetRegistry, UTransactor |
| WebSocket | FWebSocketsModule | UE built-in, cross-platform |
| Threading | FRunnable + AsyncTask | Worker thread pool; never block GameThread |
| Source control | ISourceControlModule | Perforce / Git LFS auto-checkout |

## Server

| Component | Choice | Notes |
|---|---|---|
| Language | C++23 | Modern (concepts, std::expected, ranges, coroutines, modules) |
| Build | CMake + vcpkg (manifest mode) | Cross-platform |
| MCP impl | Manual | No official C++ SDK; ~1500 LOC investment |
| HTTP server | cpp-httplib | For HTTP+SSE transport (ADR-013) |
| WebSocket | ixwebsocket | Plugin bridge (ADR-015 — replaces uWebSockets) |
| JSON | nlohmann/json + simdjson | Ergonomics + hot-path parse |
| Logging | spdlog | Structured, performant |
| Test | Catch2 | Modern macro syntax, broad ecosystem |
| Sanitizers | ASan + UBSan + TSan | Compensate for C++ memory model gaps |

## Storage

| Component | Choice | Use case |
|---|---|---|
| Knowledge graph | KuzuDB | Embedded graph DB, Cypher dialect, columnar, single-file |
| Audit log + slot index | SQLite | Tabular data, ACID, ubiquitous tooling |
| Slot data layout | Filesystem hierarchy | `~/.sage-mcp/slots/<slot_id>/` per-slot isolation |

## Transports

| Layer | Choice | Endpoint |
|---|---|---|
| Claude ↔ Server | HTTP + SSE | `http://localhost:7777/mcp` (default) |
| Server ↔ Plugin | WebSocket | `ws://localhost:7778/bridge` (default) |
| Envelope | JSON-RPC 2.0 | Both directions |
| Heartbeat | Application-level | Plugin → server, 15s interval |

## Build Tooling

| Tool | Purpose |
|---|---|
| CMake 3.25+ | Build system |
| vcpkg | C++ package manager (manifest mode) |
| clang-format | Code formatting |
| clang-tidy | Static analysis |
| Catch2 | Unit testing |
| AddressSanitizer | Memory error detection (debug builds) |
| UndefinedBehaviorSanitizer | UB detection |
| ThreadSanitizer | Race detection (weekly CI) |

## Why C++23 Over Alternatives

Detail in [ADR-001](../decisions/adr-001-tech-stack.md). Summary:

- **vs TS/Node**: ecosystem mature but runtime safety lower; distribution friction (`pkg`, `bun build`); FFI verbose.
- **vs Rust**: ergonomics great but adds a second language and a `cxx`/FFI seam; KuzuDB binding adds wrapper layer.
- **vs Python**: editor-only via `PythonScriptPlugin`, distribution friction, GIL, runtime perf.
- **vs Go**: solid but JSON handling verbose; ecosystem split for graph DB binding.

C++23 wins because:
1. User has senior Unreal C++ expertise — ergonomic gap to Rust narrows.
2. Single-language stack (plugin + server) — shared headers, no FFI seam.
3. KuzuDB primary API is C++.
4. Performance ceiling highest.
5. In-process embed remains an option (server inside editor for single-binary distribution).
