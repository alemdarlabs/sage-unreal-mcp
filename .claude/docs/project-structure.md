# Project Structure

Current source layout after ADR-018.

```text
sage-unreal-mcp/
|-- README.md
|-- CLAUDE.md
|-- AGENTS.md
|-- CMakeLists.txt
|-- vcpkg.json
|-- .claude/
|   |-- agents/
|   |-- decisions/
|   |-- docs/
|   |-- notes/
|   `-- skills/
|-- plugin/
|   |-- SageBridge.uplugin
|   `-- Source/SageBridge/
|       |-- Public/
|       |-- Private/
|       |   |-- SageBridgeSubsystem.cpp
|       |   |-- SageToolDispatch.cpp
|       |   `-- Tools/
|       `-- SageBridge.Build.cs
|-- server/
|   |-- CMakeLists.txt
|   |-- include/
|   `-- src/
|       |-- main.cpp
|       |-- audit/
|       |-- lifecycle/
|       |-- mcp/
|       |-- tools/
|       `-- transport/
|-- tests/
|   |-- CMakeLists.txt
|   |-- integration/
|   `-- unit/
|-- scripts/
`-- docs/
```

## Removed Areas

The following paths were removed with ADR-018:

- `server/src/graph/`
- `plugin/Source/SageBridge/*/Tools/SageIndexTools.*`
- Kuzu/graph smoke tests under `tests/integration/`
- `scripts/fetch-kuzu.ps1`
- `scripts/fetch-kuzu.sh`
- `third_party/kuzu` local dependency directory

## Server Modules

- `mcp/`: JSON-RPC envelope, MCP registry, schema shaping.
- `transport/`: HTTP + SSE server and WebSocket bridge server.
- `audit/`: audit and local persistence surfaces.
- `lifecycle/`: editor connection state, restart orchestration, routing support.
- `tools/`: server-side tool schemas and any local server tools.

## Plugin Module

`SageBridge` owns editor-facing Unreal operations:

- WebSocket client, reconnect, heartbeat.
- Tool dispatch and response shaping.
- GameThread-safe editor mutations.
- Reflection, AssetRegistry-backed inspection, Blueprint, material, level, animation, Niagara, UMG, gameplay, GAS, PCG, and related domain tools.

## Naming Conventions

- C++ files follow Unreal conventions: `PascalCase.h` / `PascalCase.cpp`.
- Markdown/docs use `kebab-case.md`.
- CMake targets use `kebab-case`.
- Tool names use dotted domains such as `asset.search`, `bp.full_dump`, `editor.search_log`.

## Build Output

```text
build/
|-- debug/
|   |-- bin/sage-server.exe
|   `-- tests/
|-- release/
`-- plugin/
```

There is no `sage-graph` target and no Kuzu runtime DLL in the active build output.
