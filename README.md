# Sage Unreal MCP

> **Copyright (c) 2026 alemdarlabs. All rights reserved.**
> Proprietary commercial software. Source code is private. The published
> binary will be distributed via npm (`@alemdarlabs/sage-mcp`) with
> authenticated runtime access — see ADR-016 (distribution) and the
> separate End User License Agreement (forthcoming) for terms.

AI agent integration for **Unreal Engine** via the Model Context Protocol.

Part of the **Sage** family of engine MCP servers:

- `sage-unreal-mcp` — this repo
- `sage-unity-mcp` — planned
- `sage-godot-mcp` — planned

## Status

**Phase 1–4 + Milestone 1.5b Complete** (as of 2026-04-30 · 102 commits)

- **443 MCP tool handlers** in the Unreal plugin (SageBridgeSubsystem)
- **457 tool schemas** in the C++23 server (`tools/list` fully populated; **444 of them carry an optional `_editor` parameter** for multi-editor per-call routing — see ADR-017)
- **UE-MCP parity:** 445 / 448 actions covered (99.3%) — 3 N/A (feedback + demo categories)
- **KuzuDB graph layer removed** by ADR-018; project understanding now comes from live Unreal inspection, reflection, AssetRegistry-backed tools, source search, and domain-specific diagnostics
- **Multi-editor per-call routing**: tool calls can target a specific connected editor by `_editor: "<session_id|label|instance_id|project-name>"`; falls back to active pointer or single-editor implicit; ambiguity errors when neither set
- **First real-MCP-client dogfooding loop**: 11 gaps reported and fixed in a single session (2026-04-29) — `bp.full_dump` (atomic Blueprint snapshot), `project.create_cpp_class` `bootstrap_module` (BP→C++ scaffold), `restart_editor` `rebuild_project_modules` (Mac UBT compile), schema-generator brace-init bug fix (235 schemas), and more
- Tested on UE 5.7.4 (Mac); Windows port present in code paths, first real run pending
- Both binaries build clean: `scripts/build-plugin.sh` (UAT) / `scripts/build-plugin.ps1` (Windows) + `cmake --build --preset debug`
- 18 Architectural Decision Records under [`.claude/decisions/`](.claude/decisions/)

## What this is

Most AI dev tools today are *execution layers* — they let an AI run commands. Sage focuses on source-backed Unreal operations: live editor inspection, reflection, AssetRegistry-backed discovery, source search, and safe transactional edits.

Concretely, Sage exposes Unreal Engine to MCP clients (Claude Code, Cursor, etc.) through a persistent C++ server that routes tools to connected editor instances, mediates safe transactional edits via Unreal's native UTransactor, and survives editor restarts (compile/Live Coding cycles, multi-editor sessions).

## Documentation

Codebase instructions and design docs live under `.claude/`:

- [`CLAUDE.md`](CLAUDE.md) — agent system, working rules, tech stack, build commands
- [`.claude/docs/architecture.md`](.claude/docs/architecture.md) — system topology and lifecycle
- [`.claude/docs/tech-stack.md`](.claude/docs/tech-stack.md) — language, library, build choices
- [`.claude/docs/api-spec.md`](.claude/docs/api-spec.md) — MCP tool catalog and transport protocols
- [`.claude/docs/database-schema.md`](.claude/docs/database-schema.md) — active SQLite/audit storage and retired KuzuDB notes
- [`.claude/docs/project-structure.md`](.claude/docs/project-structure.md) — folder layout and conventions
- [`.claude/docs/knowledge-graph.md`](.claude/docs/knowledge-graph.md) — retired KuzuDB graph layer and current inspection approach
- [`.claude/docs/transactions.md`](.claude/docs/transactions.md) — transaction layer detail
- [`.claude/docs/compile-coordination.md`](.claude/docs/compile-coordination.md) — Live Coding vs full restart
- [`.claude/docs/bp-cpp-conversion-pipeline.md`](.claude/docs/bp-cpp-conversion-pipeline.md) — 10-gate BP→C++ migration playbook + tool reference
- [`.claude/decisions/`](.claude/decisions/) — Architectural Decision Records (ADRs)

## Brand

"Sage" = wise agent, knowing helper. Pattern: `sage-<engine>-mcp`. The brand explicitly emphasizes the intelligence/cognition aspect, separating Sage from the execution-only crowd.
