# Project Structure

Current source layout after the repository cleanup and ADR-018 KuzuDB removal.

```text
sage-unreal-mcp/
|-- .agents/
|   `-- skills/
|-- .github/
|   `-- workflows/
|-- docs/
|   |-- adr/
|   |-- architecture/
|   |-- engineering/
|   |-- release/
|   |-- research/
|   `-- archive/
|-- npm/
|   |-- agents/
|   |-- bin/
|   |-- lib/
|   |-- scripts/
|   `-- tests/
|-- plugin/
|   |-- Config/
|   |-- Source/SageBridge/
|   |   |-- Public/
|   |   `-- Private/
|   |       `-- Tools/
|   `-- SageBridge.uplugin
|-- scripts/
|   `-- smoke/
|-- server/
|   |-- CMakeLists.txt
|   `-- src/
|       |-- audit/
|       |-- bridge/
|       |-- lifecycle/
|       |-- mcp/
|       |-- tools/
|       `-- transport/
|-- tests/
|   |-- integration/
|   `-- unit/
|-- AGENTS.md
|-- README.md
|-- BUILD.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- package.json
`-- vcpkg.json
```

## Root Policy

The repo root is reserved for source entry points, manifests, and top-level documentation. Do not place runtime logs, smoke outputs, debugger dumps, temporary diffs, generated package zips, or one-off analysis artifacts in the root.

Ignored local outputs belong under one of these locations:

- `build/`
- `dist/`
- `artifacts/`
- `logs/`
- OS temp directories

## Documentation Layout

- `docs/adr/`: Architectural Decision Records.
- `docs/architecture/`: active topology and lifecycle docs.
- `docs/engineering/`: API, project structure, build/runtime pipelines, compile coordination.
- `docs/release/`: npm, release, binary packaging, distribution docs.
- `docs/research/`: source-backed competitor and parity research.
- `docs/archive/`: historical gap logs, retired graph docs, old roadmap material.

Files under `docs/archive/` are historical. Validate against source before treating them as current.

## Server Modules

- `mcp/`: JSON-RPC envelope, MCP registry, schema shaping.
- `transport/`: HTTP/SSE server and stdio transport.
- `bridge/`: WebSocket bridge to Unreal plugins.
- `lifecycle/`: editor connection state, restart orchestration, routing support.
- `audit/`: local audit and persistence surfaces.
- `tools/`: server-side tool schemas and local server tools.

## Plugin Module

`SageBridge` owns editor-facing Unreal operations:

- WebSocket client, reconnect, and heartbeat.
- Tool dispatch and response shaping.
- GameThread-safe editor mutations.
- Reflection, AssetRegistry-backed inspection, Blueprint, material, level, animation, Niagara, UMG, gameplay, GAS, PCG, Sequencer, and related domain tools.

## Removed Areas

ADR-018 removed the active KuzuDB graph layer. The active build no longer has:

- `server/src/graph/`
- `SageIndexTools.*`
- `sage-graph` targets
- KuzuDB runtime DLL/package dependency
- graph smoke tests as active verification

Retired graph documentation is archived under `docs/archive/legacy-graph/`.
