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

During install, Sage downloads the native server and attempts to register a
global Codex MCP server named `sage` that launches `sage mcp`. If Codex is not
installed yet, run this once after installing Codex:

```powershell
sage setup codex
```

After that, open Codex from any Unreal project directory. The `sage mcp`
wrapper discovers the nearest `.uproject` from the current working directory and
passes that project context to the native server.

Install or repair the Unreal plugin for the current project:

```powershell
sage bootstrap
```

`sage bootstrap` can also target a specific project:

```powershell
sage bootstrap "<absolute-path-to-your-project.uproject>"
```

Legacy project-local configuration remains available:

```powershell
$Project = "<absolute-path-to-your-project.uproject>"
$UnrealRoot = "<absolute-path-to-your-Unreal-Engine-install>"

sage init $Project --ue-root $UnrealRoot
```

Validate a project installation:

```powershell
sage doctor --json
```

Print the packaged AI operating guide:

```powershell
sage guide codex
```

`sage bootstrap` installs `SageBridge` under `Plugins/SageBridge` and enables
the plugin in the `.uproject` without requiring `.mcp.json`. `sage init` does
the same and also writes project-local MCP config. If a previous
`Plugins/SageBridge` directory exists, the installer backs it up before copying
the new package.

## What Ships

Sage ships as two native components plus a thin npm CLI:

| Component | Role |
|---|---|
| `sage-server` | C++23 MCP server with HTTP/SSE and stdio entry points, tool schema registration, job orchestration, release packaging, and multi-editor routing. |
| `SageBridge` | Unreal Editor plugin that executes editor-safe operations through Unreal reflection, AssetRegistry, UTransactor, editor subsystems, and domain-specific tool modules. |
| `sage` npm CLI | Installer and launcher that downloads native release assets, installs the Unreal plugin, writes MCP config, and starts the server for MCP clients. |

The npm package is lightweight. During `npm install`, the CLI downloads the
native `sage-server` binary into `~/.sage-mcp` and registers Codex when
available. During `sage bootstrap` or `sage init`, it resolves or downloads the
`SageBridge` plugin package for the target Unreal project.

## Release Target

`v0.1.9` is the next release target for cross-platform server assets,
zero-config Codex onboarding, project plugin version checks, and MCP-native Sage
guidance tools. `v0.1.6` was the last Windows-only native asset release.

```powershell
npm view @alemdarlabs/sage-mcp version dist.tarball
```

Release assets:

- `sage-server-0.1.9-win32-x64.zip`
- `sage-server-0.1.9-linux-x64.tar.gz`
- `sage-server-0.1.9-darwin-arm64.tar.gz`
- `sage-server-0.1.9-darwin-x64.tar.gz`
- `sagebridge-plugin-0.1.9-source.tar.gz`
- `alemdarlabs-sage-mcp-0.1.9.tgz`
- `checksums.txt`

The next release pipeline produces platform-specific `sage-server` assets for
Windows x64, Linux x64, macOS arm64, and macOS x64, plus one platform-independent
source `SageBridge` plugin asset.

The plugin release asset is a download artifact only. Project installation
always uses the Unreal layout `Plugins/SageBridge`; version, platform, and
`source` labels never become project folder names. GitHub-hosted runners do not
include Unreal Engine, so binary `RunUAT BuildPlugin` packaging remains a
self-hosted runner concern.

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

- `server_tool_count`: 1251
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

### AI Onboarding Tools

Sage is self-describing over MCP. Agents should not need to read this README
before they can start safely.

| Tool | Purpose |
|---|---|
| `sage.about` | Product identity, version, safety model, and first-call sequence. |
| `sage.status` | Server, environment, project discovery, connected editors, and version alignment. |
| `sage.doctor` | Actionable setup/version/editor checks with repair commands. |
| `sage.project.discover` | Locate the Unreal project from `start_path`, `SAGE_PROJECT_ROOT`, or cwd. |
| `sage.capabilities` | Group tool families and recommend first read-only tools. |
| `sage.workflow.suggest` | Suggest a safe tool sequence for a natural-language task intent. |
| `sage.help` / `sage.guide` | Compact operating guide for MCP clients. |

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
sage setup codex [options]
sage guide [agent] [--json]
sage bootstrap [Project.uproject] [options]
sage init <Project.uproject> [options]
sage update
sage update <Project.uproject>
sage update --plugin <Project.uproject>
```

Common `init` options:

```powershell
--ue-root <path>         Add SAGE_UE_ROOT to generated MCP config
--codex                  Register global Sage MCP in Codex CLI
--claude                 Register the project in Claude Code
--plugin-source <path>   Install from a local SageBridge package/source tree
--mcp-config <path>      Write MCP config somewhere other than <project>/.mcp.json
--no-mcp-config          Install plugin without writing MCP config
```

`sage update` without a project only repairs or downloads the machine-level
native server for the installed npm package. `sage update <Project.uproject>` is
the project-level update path: it verifies the server binary, refuses to copy
`SageBridge` while that Unreal project is open, installs the matching plugin
package under `Plugins/SageBridge`, enables the plugin in the `.uproject`, and
prints the installed plugin version. `sage doctor` reports the installed
`SageBridge` version versus the current Sage CLI version and suggests
`sage update "<Project.uproject>"` when they diverge. It also checks npm latest
unless `--offline` or `SAGE_SKIP_NPM_LATEST=1` is set, and suggests
`npm install -g @alemdarlabs/sage-mcp@latest` when the local CLI is stale.

Common `guide` options:

```powershell
--agent <name>           codex, claude, cursor, or generic
--json                   Print {agent,path,content}
```

Common `setup codex` options:

```powershell
--codex-name <name>      Codex MCP server name. Default: sage
--codex-command <path>   Codex executable path. Default: codex
--no-replace             Do not replace an existing non-Sage Codex MCP entry
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
| `SAGE_SKIP_CODEX_SETUP` | Skip postinstall Codex MCP registration. |
| `SAGE_CODEX_SETUP_STRICT` | Set to `1` to make postinstall Codex registration failure fatal. |
| `SAGE_CODEX_COMMAND` | Override the Codex executable used by setup commands. |
| `SAGE_SKIP_NPM_LATEST` | Skip `sage doctor` npm registry latest-version check. |
| `SAGE_PROJECT_ROOT` | Unreal project root passed to the server or discovered by `sage mcp`. |
| `SAGE_REPO_ROOT` | Workspace root passed to the server. Defaults to the discovered project root when available. |
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

1. Builds `sage-server` on Windows x64, Linux x64, macOS arm64, and macOS x64.
2. Builds and runs native tests on each server runner.
3. Runs npm smoke and release checks on each server runner.
4. Packages one platform-specific server asset per runner.
5. Packages one platform-independent source `SageBridge` plugin asset.
6. Merges artifacts, writes `checksums.txt`, and uploads GitHub Release assets.
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
