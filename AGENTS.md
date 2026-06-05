# Sage Unreal MCP - Agent Guide

This file is the canonical working guide for the repository. The old hidden
workspace layout has been removed. Durable documentation lives under `docs/`;
active Codex skill definitions live under `.agents/skills/`.

## Communication

- Speak Turkish with the user unless they explicitly ask for another language.
  Code identifiers, commands, paths, commit messages, and public documentation
  can remain English.
- If the user asks for approval before changes, do not edit files. Provide the
  plan and rationale only.
- If the user says not to build, start the server, launch PIE, or perform a
  similar action, that boundary stays closed until the user explicitly reopens
  it.
- Report verification claims separately. File edits, build success, tests,
  deployment, server health, and runtime proof are different facts.

## Skill Routing

Use the most relevant skill under `.agents/skills/` for the dominant domain of
the task. If a specialized skill is missing, inspect source and proceed with the
same discipline.

| Domain | Preferred skill or approach |
|---|---|
| Unreal C++ plugin, UCLASS, AssetRegistry, Slate, Live Coding | Source-backed Unreal inspection; `code-review` for reviews |
| C++23 server, CMake, vcpkg, packaging | `devops-assistant`; `code-review` when implementation risk matters |
| MCP protocol, JSON-RPC, tool schema design | Source inspection plus `knowledge-structuring` |
| Tool parity and competitor repository comparisons | `competitive-intelligence`, `deep-research-synthesizer` |
| Documentation, ADRs, information architecture | `knowledge-structuring`, `tone-style-enforcer`, `scqa-writing` |
| CI/CD, release, npm distribution | `devops-assistant` |
| Security, auth, sandboxing | Source-backed security review |
| Test strategy and regression risk | `code-review` with a QA lens |

## Product State

Sage Unreal MCP is a C++23 MCP server plus the `SageBridge` Unreal Editor
plugin. The system no longer uses the KuzuDB-backed graph runtime; ADR-018
removed that layer. Project understanding now comes from live Unreal
inspection, reflection, AssetRegistry queries, source search, logs, and
domain-specific diagnostics.

Current source audit command:

```powershell
.\scripts\audit-tools.ps1 -Json
```

Current audit result, verified on 2026-06-05:

- `server_tool_count`: 1251
- `plugin_handler_count`: 1191
- `plugin_without_schema`: 0
- `schema_stub_count`: 0
- `plugin_not_implemented_count`: 0
- `schema_without_plugin`: server-only `jobs.*` management tools

The repository may be public, but that does not make it open source. Do not
assume permissive usage rights unless a license explicitly grants them. The
distribution model is npm-first binary packaging, with runtime authentication
and licensing planned as a separate product layer.

## Canonical Layout

```text
sage-unreal-mcp/
|-- .agents/                 # Codex skill definitions
|-- .github/                 # CI and release workflows
|-- docs/
|   |-- adr/                 # Architectural Decision Records
|   |-- architecture/        # Active architecture docs
|   |-- engineering/         # API, build, project structure, pipelines
|   |-- release/             # npm and binary distribution notes
|   |-- research/            # Reference parity and market research
|   `-- archive/             # Historical notes, retired designs, gap logs
|-- npm/                     # npm package wrapper, agent guides, and postinstall logic
|-- plugin/                  # Unreal SageBridge plugin
|-- scripts/                 # Build, package, smoke, audit helpers
|-- server/                  # C++23 MCP server
|-- tests/                   # Catch2 unit and integration tests
|-- AGENTS.md                # This file
|-- README.md
|-- BUILD.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- package.json
`-- vcpkg.json
```

Do not leave temporary logs, smoke output, debugger dumps, or one-off analysis
files in the repository root. Keep them under ignored artifact/log/temp/build
locations.

## Important Documents

- [Architecture](docs/architecture/architecture.md)
- [Tech Stack](docs/engineering/tech-stack.md)
- [API Spec](docs/engineering/api-spec.md)
- [Project Structure](docs/engineering/project-structure.md)
- [Compile Coordination](docs/engineering/compile-coordination.md)
- [Blueprint to C++ Pipeline](docs/engineering/bp-cpp-conversion-pipeline.md)
- [Transactions](docs/engineering/transactions.md)
- [ADR Index](docs/adr/)
- [npm Distribution Plan](docs/release/npm-distribution-plan.md)
- [Reference Tool Parity Research](docs/research/tool-parity/)

Historical gap logs and old working notes live under
`docs/archive/working-notes/`. Treat them as evidence, not current truth. Verify
against source and audit scripts before acting on them.

## AI Client Onboarding Contract

Sage must be discoverable by any MCP-capable AI client without prior private
context. The npm package carries public agent guides under `npm/agents/`, and
the server exposes MCP-native guidance tools.

When a fresh Codex, Claude, Cursor, or other MCP client is told to use Sage in
an Unreal project, the expected first MCP calls are:

1. `sage.about`
2. `sage.project.discover`
3. `sage.doctor`
4. `sage.capabilities`
5. `list_editors`
6. `sage.workflow.suggest` with the user's concrete task intent

If the MCP client cannot find its instructions, the CLI fallback is:

```powershell
sage guide codex
sage guide generic
sage doctor <Project.uproject>
```

`sage.doctor` is the authoritative setup/version check for an installed
machine: it checks local package state, npm latest version when online, project
plugin install state, project plugin version, and optional project MCP config.
If it reports a stale plugin, the fix is `sage update <Project.uproject>`. If
it reports a stale CLI package, the fix is
`npm install -g @alemdarlabs/sage-mcp@latest`.

## Build And Run

Do not run builds unless the user explicitly asks for them.

```powershell
# Server
$env:VCPKG_ROOT = "$HOME\vcpkg"
cmake --build --preset debug --target sage-server

# Plugin package
.\scripts\build-plugin.ps1

# Tool audit; does not require a build
.\scripts\audit-tools.ps1 -Json
```

Manual server debugging:

```powershell
$env:SAGE_REPO_ROOT = (Get-Location).Path
$env:SAGE_UE_ROOT = "<absolute-path-to-your-Unreal-Engine-install>"
$env:SAGE_LOG_LEVEL = "info"
.\build\debug\bin\sage-server.exe
```

npm release checks:

```powershell
npm run package:assets
npm run release:check
```

GitHub Actions npm publishing requires the `NPM_TOKEN` repository secret. If
the secret is missing, the publish step should fail fast. GitHub-hosted Windows
runners do not include Unreal Engine, so the hosted release workflow packages a
source `SageBridge` plugin asset. Binary `RunUAT BuildPlugin` packaging requires
a self-hosted Windows runner with Unreal Engine installed.

## Production Project Guard

Destructive operations against real Unreal projects require discipline:

- Take `bp.full_dump` before major Blueprint mutation, reparenting, deletion, or
  conversion.
- Do not proceed with tools that require `confirmed:true` until the user gives
  explicit approval.
- Do not automatically run manual recursive deletes, wipe plugin `Source/`, or
  remove `.uproject` `Modules` entries in production projects.
- Pass `_editor` explicitly in multi-editor workflows.
- Check source control state before broad mutation.
- When deployment is requested for target projects, report build, copy, enable,
  and hash verification as separate steps.

For a different Codex working inside an arbitrary Unreal project, "use Sage" or
"deploy Sage" does not mean manually copying from an old checkout. The expected
flow is:

1. Discover the project with `sage.project.discover` or `sage doctor`.
2. Repair install/version drift with `sage update <Project.uproject>`.
3. Refuse plugin update while the matching Unreal Editor process is open.
4. Verify editor connectivity with `list_editors` or `wait_for_editor`.
5. Use `sage.workflow.suggest` to choose the narrowest safe read/write tool
   sequence for the user's actual project task.

For deployment from this repository into a production Unreal project, the
Windows plugin deployment contract is still: build/package when the user allows
builds, copy `SageBridge.uplugin`, `Binaries/<Platform>/`, and `Source/`, ensure
the target `.uproject` enables `SageBridge`, then hash-check the deployed
`UnrealEditor-SageBridge.dll` against the packaged DLL before claiming success.

## Engineering Rules

- Read real source files first. Do not rely on old notes or memory alone.
- The repository may be dirty. Do not revert user-owned changes.
- Use `apply_patch` for manual file edits. Native PowerShell or git commands
  are acceptable for mechanical moves or deletes.
- On Windows, verify recursive delete or move targets are inside the intended
  workspace before executing.
- Unreal editor mutation code must marshal to the Game Thread.
- For MCP schema changes, verify parity with `tools/list` and
  `scripts/audit-tools.ps1`.
- C++ plugin deployment has a three-part rule: `.uplugin`,
  `Binaries/<Platform>/`, and `Source/`.
- Repository skills must be Windows-first or genuinely cross-platform. Do not
  make macOS-only instructions canonical for production workflows.
- If build or tests ran, report their result. If they did not run, say so.

## Closeout Check

Minimum check before finishing a task:

```powershell
git status --short
rg --pcre2 --hidden -n "[\x{00E7}\x{011F}\x{0131}\x{00F6}\x{015F}\x{00FC}\x{00C7}\x{011E}\x{0130}\x{00D6}\x{015E}\x{00DC}]" -g "*.md" -g "*.txt" -g "!build/**" -g "!node_modules/**" -g "!.git/**"
.\scripts\audit-tools.ps1 -Json
```

Builds run only when the user says it is the right time.
