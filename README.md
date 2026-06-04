# Sage Unreal MCP

[![npm](https://img.shields.io/npm/v/@alemdarlabs/sage-mcp?label=npm)](https://www.npmjs.com/package/@alemdarlabs/sage-mcp)
[![release](https://img.shields.io/github/v/release/alemdarlabs/sage-unreal-mcp?label=release)](https://github.com/alemdarlabs/sage-unreal-mcp/releases)
[![workflow](https://github.com/alemdarlabs/sage-unreal-mcp/actions/workflows/release-assets.yml/badge.svg)](https://github.com/alemdarlabs/sage-unreal-mcp/actions/workflows/release-assets.yml)

> Copyright (c) 2026 alemdarlabs. All rights reserved.
>
> Source-visible proprietary commercial software. Public repository visibility
> and public npm distribution do not grant an open-source license.

Sage Unreal MCP is a native Model Context Protocol integration for Unreal
Engine. It lets AI development clients inspect, reason about, and operate on an
Unreal project through a C++ MCP server and an Unreal Editor plugin.

Sage is designed for production Unreal projects: multi-editor routing, native
reflection, AssetRegistry-backed discovery, transactional editor operations,
source search, compile coordination, logs, crash forensics, and domain-specific
tooling live behind one MCP surface.

## Install

```powershell
npm install -g @alemdarlabs/sage-mcp
sage doctor
```

Install the Unreal plugin into a project and write project-local MCP
configuration:

```powershell
$Project = "<absolute-path-to-your-project.uproject>"
$UnrealRoot = "<absolute-path-to-your-Unreal-Engine-install>"

sage init $Project --ue-root $UnrealRoot
```

Register the same project with a client:

```powershell
# Codex CLI
sage init $Project --ue-root $UnrealRoot --codex

# Claude Code
sage init $Project --ue-root $UnrealRoot --claude
```

Validate a project installation:

```powershell
sage doctor $Project --json
```

`sage init` installs `SageBridge` under `Plugins/SageBridge`, enables the plugin
in the `.uproject`, and writes `.mcp.json`. If a previous `Plugins/SageBridge`
directory exists, the installer backs it up before copying the new package.

## What Ships

Sage ships as two native components plus a thin npm CLI:

| Component | Role |
|---|---|
| `sage-server` | C++23 MCP server with HTTP/SSE and stdio entry points, tool schema registration, job orchestration, release packaging, and multi-editor routing. |
| `SageBridge` | Unreal Editor plugin that executes editor-safe operations through Unreal reflection, AssetRegistry, UTransactor, editor subsystems, and domain-specific tool modules. |
| `sage` npm CLI | Installer and launcher that downloads native release assets, installs the Unreal plugin, writes MCP config, and starts the server for MCP clients. |

The npm package is lightweight. During `npm install`, the CLI downloads the
native `sage-server` binary into `~/.sage-mcp`. During `sage init`, it resolves
or downloads the `SageBridge` plugin package for the target Unreal project.

## Current Release

`v0.1.0` is published on npm and GitHub Releases.

```powershell
npm view @alemdarlabs/sage-mcp version dist.tarball
```

Release assets:

- `sage-server-0.1.0-win32-x64.zip`
- `sagebridge-plugin-0.1.0-win32-x64.zip`
- `alemdarlabs-sage-mcp-0.1.0.tgz`
- `checksums.txt`

GitHub-hosted Windows runners do not include Unreal Engine. The hosted release
workflow packages the source `SageBridge` plugin asset; binary `RunUAT
BuildPlugin` packaging requires a self-hosted Windows runner with Unreal Engine
installed.

## Architecture

```text
MCP client
  |  stdio or HTTP/SSE
  v
sage-server
  |  WebSocket bridge
  v
SageBridge Unreal Editor plugin
  |  Unreal reflection, AssetRegistry, editor subsystems, transactions
  v
Unreal project
```

Key properties:

- MCP clients talk to `sage-server`, not directly to Unreal.
- `SageBridge` connects from the editor to the server over WebSocket.
- Editor-scoped tools can target a specific editor session through `_editor`.
- Long-running and mutating operations are modeled explicitly.
- Project understanding is source-backed and live-editor-backed, not based on a
  retired persistent graph runtime.

ADR-018 removed the KuzuDB graph layer. Current project understanding uses live
Unreal inspection, reflection, AssetRegistry-backed discovery, source search,
logs, and specialized domain tools.

## Tool Surface

Current source audit, generated with `scripts/audit-tools.ps1 -Json` on
2026-06-05:

- `server_tool_count`: 1243
- `plugin_handler_count`: 1191
- `plugin_without_schema`: 0
- `schema_stub_count`: 0
- `plugin_not_implemented_count`: 0
- `schema_without_plugin`: server-only `jobs.*` tools

The audit script is the source-level parity check for schemas, plugin handlers,
stubs, and placeholder implementations.

```powershell
.\scripts\audit-tools.ps1 -Json
```

## Safety Model

Sage is intended for real Unreal projects, not only throwaway samples.

- Destructive tools require explicit confirmation.
- Unreal editor mutation code must execute on the Game Thread.
- Project plugin installation backs up an existing `Plugins/SageBridge`
  directory before replacing it.
- Production Blueprint migration or deletion workflows should take a
  `bp.full_dump` first.
- Multi-editor workflows should pass `_editor` explicitly when more than one
  editor can be connected.
- Build, deploy, server health, asset save, and runtime proof are separate
  verification claims.

The CLI does not bypass source control. Check your Unreal project workspace
before running install or mutation flows against production content.

## CLI Reference

```powershell
sage --version
sage doctor [Project.uproject] [--json]
sage mcp [server args...]
sage server [--http] [server args...]
sage init <Project.uproject> [options]
sage update
sage update --plugin <Project.uproject>
```

Common `init` options:

```powershell
--ue-root <path>         Add SAGE_UE_ROOT to generated MCP config
--codex                  Register the project in Codex CLI
--claude                 Register the project in Claude Code
--plugin-source <path>   Install from a local SageBridge package/source tree
--mcp-config <path>      Write MCP config somewhere other than <project>/.mcp.json
--no-mcp-config          Install plugin without writing MCP config
```

## Configuration

| Variable | Purpose |
|---|---|
| `SAGE_DATA_DIR` | Override the local Sage cache directory. Defaults to `~/.sage-mcp`. |
| `SAGE_SERVER_PATH` | Use an explicit native server binary. |
| `SAGE_PLUGIN_SOURCE` | Use an explicit unpacked SageBridge plugin source/package directory. |
| `SAGE_BINARY_BASE_URL` | Override the GitHub Release base URL for native assets. |
| `SAGE_PLUGIN_BASE_URL` | Override the GitHub Release base URL for plugin assets. |
| `SAGE_BINARY_URL` | Override the exact server archive URL. |
| `SAGE_PLUGIN_URL` | Override the exact plugin archive URL. |
| `SAGE_SKIP_DOWNLOAD` | Skip postinstall native binary download for offline or development installs. |
| `SAGE_POSTINSTALL_STRICT` | Set to `0` to make postinstall download failure non-fatal. |
| `SAGE_PROJECT_ROOT` | Unreal project root passed to the server in generated MCP config. |
| `SAGE_REPO_ROOT` | Repository/project root passed to the server in generated MCP config. |
| `SAGE_UE_ROOT` | Unreal Engine install root used by build and editor workflows. |
| `SAGE_LOG_LEVEL` | Runtime logging level. |
| `SAGE_HTTP_HOST`, `SAGE_HTTP_PORT` | HTTP/SSE MCP bind address. Defaults to `127.0.0.1:7777`. |
| `SAGE_WS_HOST`, `SAGE_WS_PORT` | Unreal plugin WebSocket bind address. Defaults to `127.0.0.1:7778`. |

## Development

Requirements:

- Node.js 18 or newer
- CMake 3.25 or newer
- vcpkg
- Ninja
- Visual Studio toolchain on Windows
- Unreal Engine for `RunUAT BuildPlugin` packaging

Build the server:

```powershell
$env:VCPKG_ROOT = "$HOME\vcpkg"
cmake --preset debug
cmake --build --preset debug --target sage-server
```

Run native tests:

```powershell
cmake --build --preset debug --target sage-tests
ctest --preset debug --output-on-failure
```

Package the Unreal plugin locally:

```powershell
$env:SAGE_UE_ROOT = "<absolute-path-to-your-Unreal-Engine-install>"
.\scripts\build-plugin.ps1
```

Run npm packaging checks:

```powershell
npm test
npm run test:global
npm run release:check
```

Start the server manually for local debugging:

```powershell
$env:SAGE_REPO_ROOT = (Get-Location).Path
$env:SAGE_UE_ROOT = "<absolute-path-to-your-Unreal-Engine-install>"
$env:SAGE_LOG_LEVEL = "info"
.\build\debug\bin\sage-server.exe
```

## Release Process

Release tags are `v<package.json version>`.

On a tag push, GitHub Actions:

1. Configures the Windows MSVC toolchain.
2. Builds `sage-server`.
3. Builds and runs native tests.
4. Runs npm smoke and release checks.
5. Packages release assets.
6. Uploads GitHub Release assets.
7. Publishes `@alemdarlabs/sage-mcp` to npm.

`NPM_TOKEN` must be configured as a GitHub repository secret. For accounts with
2FA, the token must support CI publish workflows.

## Repository Layout

```text
sage-unreal-mcp/
|-- .agents/                 # Codex skill definitions
|-- .github/                 # CI and release workflows
|-- docs/
|   |-- adr/                 # Architectural Decision Records
|   |-- architecture/        # Active architecture docs
|   |-- engineering/         # API, build, project structure, pipelines
|   |-- release/             # npm and binary distribution notes
|   |-- research/            # Tool parity and product research
|   `-- archive/             # Historical notes and retired designs
|-- npm/                     # npm CLI, installer, postinstall, smoke tests
|-- plugin/                  # Unreal SageBridge plugin source
|-- scripts/                 # Build, package, audit, smoke helpers
|-- server/                  # C++23 MCP server
|-- tests/                   # Catch2 unit and integration tests
|-- AGENTS.md                # Canonical agent and development guide
|-- BUILD.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- package.json
`-- vcpkg.json
```

Runtime logs, smoke output, debugger dumps, release artifacts, and temporary
analysis files should not live in the repository root. Use ignored artifact,
log, temp, or build directories.

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

## License

This repository is source-visible proprietary software. There is no open-source
license grant in this repository. Do not treat public repository access, npm
package availability, or GitHub Release availability as permission to copy,
fork, resell, sublicense, or redistribute the source code outside alemdarlabs
terms.
