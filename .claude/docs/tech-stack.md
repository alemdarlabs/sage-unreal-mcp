# Tech Stack

Per-layer technology choices and rationale. ADR-018 removes KuzuDB from the active stack.

## Plugin

| Component | Choice | Notes |
|---|---|---|
| Language | Unreal C++ | UPlugin format; mandatory for engine reflection access |
| API surface | UE 5.x core/editor APIs | UCLASS/UPROPERTY/UFUNCTION reflection, AssetRegistry, UTransactor, editor subsystems |
| WebSocket | FWebSocketsModule | UE built-in, cross-platform client |
| Threading | FRunnable + AsyncTask / GameThread marshaling | Worker work stays off GameThread; editor mutations marshal back |
| Source control | ISourceControlModule | Perforce / Git style project workflows |

## Server

| Component | Choice | Notes |
|---|---|---|
| Language | C++23 | Modern C++ with strong Unreal/C++ alignment |
| Build | CMake + vcpkg manifest mode | Cross-platform native binary build |
| MCP implementation | Manual | No official C++ SDK; protocol surface is explicit |
| HTTP server | cpp-httplib | HTTP + SSE transport |
| WebSocket | ixwebsocket | Plugin bridge |
| JSON | nlohmann/json + simdjson | Ergonomic construction plus hot-path parsing options |
| Logging | spdlog | Structured logging |
| Tests | Catch2 | Unit/integration style native tests |
| Sanitizers | ASan / UBSan / TSan where supported | Debug and CI hardening |

## Storage

| Component | Choice | Use case |
|---|---|---|
| Audit / slot metadata | SQLite / filesystem | Durable local server state where needed |
| Project understanding | Live Unreal APIs and source-backed tools | AssetRegistry, reflection, source search, domain diagnostics |
| Retired graph DB | None | KuzuDB removed by ADR-018 |

## Transports

| Layer | Choice | Endpoint |
|---|---|---|
| MCP client to server | HTTP + SSE | `http://localhost:7777/mcp` |
| Server to plugin | WebSocket | `ws://localhost:7778/bridge` |
| Envelope | JSON-RPC 2.0 | Both directions |
| Heartbeat | Application-level | Plugin to server |

## Build Tooling

| Tool | Purpose |
|---|---|
| CMake 3.25+ | Build system |
| vcpkg | C++ package manager |
| clang-format | Code formatting |
| clang-tidy | Static analysis |
| Catch2 | Native tests |
| Unreal Automation Tool | Plugin packaging |

## Why C++23

- Same language family as Unreal plugin work.
- Native binary distribution without Node/Python runtime friction.
- Strong control over transport, schema shaping, and packaging.
- No FFI boundary between server-side native code and Unreal-oriented C++ concepts.
- KuzuDB is no longer a reason for the language choice; the active justification is Unreal-native integration and commercial binary distribution.
