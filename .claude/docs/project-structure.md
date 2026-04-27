# Project Structure

Project layout. Code-side paths TBD until implementation phase.

## Directory Layout

```
sage-unreal-mcp/
├── CLAUDE.md                       # Codebase instructions for Claude
├── README.md                       # Public-facing description
├── LICENSE                         # TBD
├── .clang-format                   # Code style (TBD)
├── .clang-tidy                     # Static analysis (TBD)
├── .gitignore
│
├── .claude/                        # Claude Code workspace metadata
│   ├── agents/                     # 13 expert agent definitions
│   ├── decisions/                  # ADR documents (one per decision area)
│   ├── docs/                       # This directory; system docs
│   ├── notes/                      # Scratch, todo, lessons
│   └── skills/                     # Automation skills
│
├── plugin/                         # Unreal C++ plugin (TBD)
│   ├── SageBridge.uplugin
│   ├── Source/
│   │   └── SageBridge/
│   │       ├── Public/
│   │       ├── Private/
│   │       └── SageBridge.Build.cs
│   └── Resources/
│
├── server/                         # C++23 MCP server (TBD)
│   ├── CMakeLists.txt
│   ├── vcpkg.json
│   ├── src/
│   │   ├── main.cpp
│   │   ├── mcp/                    # Manual MCP impl
│   │   ├── transport/              # HTTP+SSE, WebSocket
│   │   ├── graph/                  # KuzuDB layer + GraphStore trait
│   │   ├── audit/                  # SQLite audit
│   │   ├── lifecycle/              # Editor lifecycle manager
│   │   └── tools/                  # Tool implementations
│   └── include/
│
├── tests/                          # Catch2 tests (TBD)
│   ├── CMakeLists.txt
│   ├── unit/
│   ├── integration/                # Real plugin + real server
│   └── fixtures/                   # Test UE projects
│
├── migrations/                     # Schema migrations (TBD)
│   ├── 0001_initial.cypher
│   └── 0001_initial.sql
│
└── docs/                           # External / public-facing docs (TBD)
    ├── getting-started.md
    └── deployment.md
```

## Naming Conventions

- **Files**: `PascalCase.h` / `PascalCase.cpp` for C++ (UE convention), `kebab-case.md` for docs
- **C++ classes**: `PascalCase` (`FSageServer`, UE `U`/`A`/`F` prefix for plugin types)
- **C++ methods**: `PascalCase` for public, `lowerCamelCase` allowed for impl detail (consistent within file)
- **Variables**: `lowerCamelCase`
- **Constants**: `UPPER_SNAKE_CASE`
- **Enums**: `PascalCase` for type, `PascalCase` for values
- **CMake targets**: `kebab-case` (`sage-server`, `sage-mcp-impl`)
- **vcpkg ports**: standard kebab (`spdlog`, `nlohmann-json`)

## Module Organization

### Server (C++23)

- `mcp/` — protocol implementation (JSON-RPC envelope, capability handshake, tool registration)
- `transport/` — HTTP+SSE server, WebSocket server (separate concern from MCP semantics)
- `graph/` — `GraphStore` trait + `KuzuGraphStore` impl
- `audit/` — `AuditLog` interface + SQLite impl
- `lifecycle/` — editor connection state machine, restart orchestration
- `tools/` — one file per tool group (modification, indexing, slot management, ...)
- `domain/` — DTOs, value types (Editor, Slot, TransactionId, AssetRef)

### Plugin (Unreal C++)

`SageBridge` module (single, may split as needed):
- `WebSocketClient` — connection, reconnect, heartbeat
- `RegistryListener` — AssetRegistry event subscription
- `TransactionWrapper` — FScopedTransaction integration
- `IndexerWorker` — FRunnable-based indexing thread pool
- `ToolDispatch` — incoming tool call execution
- `ReflectionTraverser` — UClass/UProperty/UFunction walking

## Build Output Layout

```
build/
├── debug/
│   ├── sage-server          (with sanitizers)
│   └── tests/
└── release/
    └── sage-server
```

## Configuration

Configuration via TOML: `sage.toml` next to the binary, or path via `SAGE_CONFIG`. Environment variables override file values. See [README](../../README.md) for env vars.
