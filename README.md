# Sage Unreal MCP

> Copyright (c) 2026 alemdarlabs. All rights reserved.
>
> Source-visible proprietary commercial software. Public repository visibility does not grant an open-source license. Binary distribution is npm-first; runtime auth/licensing is a separate product layer.

Sage Unreal MCP connects AI development clients to Unreal Engine through the Model Context Protocol. It is built from two native pieces:

- `sage-server`: a C++23 MCP server with HTTP/SSE, stdio-friendly packaging, schema registration, job orchestration, and multi-editor routing.
- `SageBridge`: an Unreal Editor plugin that executes editor-safe operations through Unreal reflection, AssetRegistry, UTransactor, editor subsystems, and domain-specific tooling.

## Status

Current source audit, generated with `scripts/audit-tools.ps1 -Json` on 2026-06-04:

- 1243 server tool schemas
- 1191 Unreal plugin handlers
- 0 plugin handlers missing server schema
- 0 schema stubs
- 0 plugin `[NOT IMPLEMENTED]` handlers
- 18 ADRs under [`docs/adr/`](docs/adr/)

ADR-018 removed the KuzuDB graph layer. Sage now uses live Unreal inspection, reflection, AssetRegistry-backed discovery, source search, logs, and specialized domain tools for project understanding.

## Quick Install Target

The intended distribution flow is npm-first:

```powershell
npm install -g @alemdarlabs/sage-mcp
sage init D:\GameDev\Kale\Kale.uproject --ue-root "C:\Program Files\Epic Games\UE_5.7"
```

`sage init` installs `SageBridge` into the Unreal project, enables it in the `.uproject`, and writes client MCP configuration. For Codex registration:

```powershell
sage init D:\GameDev\Kale\Kale.uproject --ue-root "C:\Program Files\Epic Games\UE_5.7" --codex
```

For Claude Code registration:

```powershell
sage init D:\GameDev\Kale\Kale.uproject --ue-root "C:\Program Files\Epic Games\UE_5.7" --claude
```

Manual development mode remains available:

```powershell
$env:SAGE_REPO_ROOT = "D:\Steamworks\sage-unreal-mcp"
$env:SAGE_UE_ROOT = "C:\Program Files\Epic Games\UE_5.7"
$env:SAGE_LOG_LEVEL = "info"
.\build\debug\bin\sage-server.exe
```

## Development

```powershell
# Server
$env:VCPKG_ROOT = "$HOME\vcpkg"
cmake --build --preset debug --target sage-server

# Unreal plugin package
.\scripts\build-plugin.ps1

# Source-level tool parity audit
.\scripts\audit-tools.ps1 -Json
```

Release assets are produced with:

```powershell
npm run package:assets
npm run release:check
```

Tag pushes publish through GitHub Actions. npm publish requires the `NPM_TOKEN` repository secret. The hosted workflow packages the source `SageBridge` plugin asset; `RunUAT BuildPlugin` requires a self-hosted Windows runner with Unreal Engine installed.

## Repository Layout

```text
sage-unreal-mcp/
|-- .agents/                 # Codex skills
|-- .github/                 # CI and release workflows
|-- docs/                    # ADRs, architecture, engineering, release, research
|-- npm/                     # npm wrapper, installer, CLI tests
|-- plugin/                  # Unreal SageBridge plugin
|-- scripts/                 # build, package, audit, smoke helpers
|-- server/                  # C++23 MCP server
|-- tests/                   # native tests
|-- AGENTS.md                # canonical agent/development guide
|-- BUILD.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- package.json
`-- vcpkg.json
```

Runtime logs, smoke output, debugger dumps, release artifacts, and temporary analysis files must not live in the repo root. Use ignored artifact/log locations instead.

## Documentation

- [Agent Guide](AGENTS.md)
- [Architecture](docs/architecture/architecture.md)
- [Tech Stack](docs/engineering/tech-stack.md)
- [API Spec](docs/engineering/api-spec.md)
- [Project Structure](docs/engineering/project-structure.md)
- [Compile Coordination](docs/engineering/compile-coordination.md)
- [Blueprint to C++ Pipeline](docs/engineering/bp-cpp-conversion-pipeline.md)
- [Transactions](docs/engineering/transactions.md)
- [ADRs](docs/adr/)
- [npm Distribution Plan](docs/release/npm-distribution-plan.md)
- [Reference Tool Parity Research](docs/research/tool-parity/)

Historical notes and retired designs are kept under `docs/archive/`.
