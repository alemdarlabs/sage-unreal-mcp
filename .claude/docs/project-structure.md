# Project Structure

Gerçek proje yapısı (Phase 4 sonrası · 2026-04-28).

## Directory Layout

```
sage-unreal-mcp/
├── CLAUDE.md                       # Codebase instructions for Claude
├── README.md                       # Public-facing description
├── LICENSE                         # Apache-2.0 (ADR-016 önerisi)
├── .clang-format                   # Code style (repo kökünde)
├── .clang-tidy                     # Static analysis (repo kökünde)
├── .gitignore
│
├── .claude/                        # Claude Code workspace metadata
│   ├── agents/                     # 13 expert agent definitions
│   ├── decisions/                  # ADR documents (one per decision area)
│   ├── docs/                       # This directory; system docs
│   ├── notes/                      # Scratch, todo, lessons
│   └── skills/                     # Automation skills
│
├── plugin/                         # Unreal C++ plugin (Phase 1-4 complete)
│   ├── SageBridge.uplugin
│   ├── Source/
│   │   └── SageBridge/
│   │       ├── Public/             # Header'lar
│   │       ├── Private/
│   │       │   ├── SageBridgeSubsystem.cpp
│   │       │   ├── SageToolDispatch.cpp
│   │       │   └── Tools/          # 32 dosya · 443 handler
│   │       │       ├── SageActorTools.cpp
│   │       │       ├── SageAnimationTools.cpp  (46 tool)
│   │       │       ├── SageAssetAdvancedTools.cpp
│   │       │       ├── SageAssetTools.cpp
│   │       │       ├── SageAudioTools.cpp      (5 tool)
│   │       │       ├── SageBlueprintTools.cpp
│   │       │       ├── SageEditorAutomationTools.cpp
│   │       │       ├── SageFoliageTools.cpp    (7 tool)
│   │       │       ├── SageGameplayTools.cpp   (45 tool)
│   │       │       ├── SageGasTools.cpp        (9 tool)
│   │       │       ├── SageLandscapeTools.cpp  (11 tool)
│   │       │       ├── SageLevelTools.cpp      (22 tool)
│   │       │       ├── SageMaterialGraphTools.cpp
│   │       │       ├── SageNetworkingTools.cpp (11 tool)
│   │       │       ├── SageNiagaraTools.cpp    (26 tool)
│   │       │       ├── SagePcgTools.cpp        (16 tool)
│   │       │       ├── SageWidgetTools.cpp
│   │       │       └── ...
│   │       └── SageBridge.Build.cs
│   └── Resources/
│
├── server/                         # C++23 MCP server (Phase 1-4 complete, 454 şema)
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
├── tests/                          # Catch2 unit tests (server tarafı; 35 test)
│   ├── CMakeLists.txt
│   └── unit/
│
└── docs/                           # External / public-facing docs (başlatılmadı)
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
