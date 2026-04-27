# Sage Unreal MCP

AI agent integration for **Unreal Engine** via the Model Context Protocol.

Part of the **Sage** family of engine MCP servers:

- `sage-unreal-mcp` — this repo
- `sage-unity-mcp` — planned
- `sage-godot-mcp` — planned

## Status

Pre-implementation. Architecture design phase (started 2026-04-27).

## What this is

Most AI dev tools today are *execution layers* — they let an AI run commands. Sage adds an **intelligence layer** alongside execution: an agent that *understands* the engine project (asset graph, class hierarchy, reference topology, impact analysis) and uses that understanding to give precise, safe answers and edits.

Concretely, Sage exposes Unreal Engine to MCP clients (Claude Code, Cursor, etc.) through a persistent C++ server that maintains a Kuzu-backed knowledge graph of the project, mediates safe transactional edits via Unreal's native UTransactor, and survives editor restarts (compile/Live Coding cycles, multi-editor sessions).

## Documentation

Codebase instructions and design docs live under `.claude/`:

- [`CLAUDE.md`](CLAUDE.md) — agent system, working rules, tech stack, build commands
- [`.claude/docs/architecture.md`](.claude/docs/architecture.md) — system topology and lifecycle
- [`.claude/docs/tech-stack.md`](.claude/docs/tech-stack.md) — language, library, build choices
- [`.claude/docs/api-spec.md`](.claude/docs/api-spec.md) — MCP tool catalog and transport protocols
- [`.claude/docs/database-schema.md`](.claude/docs/database-schema.md) — KuzuDB graph + SQLite tables
- [`.claude/docs/project-structure.md`](.claude/docs/project-structure.md) — folder layout and conventions
- [`.claude/docs/knowledge-graph.md`](.claude/docs/knowledge-graph.md) — 3-tier indexing strategy
- [`.claude/docs/transactions.md`](.claude/docs/transactions.md) — transaction layer detail
- [`.claude/docs/compile-coordination.md`](.claude/docs/compile-coordination.md) — Live Coding vs full restart
- [`.claude/decisions/`](.claude/decisions/) — Architectural Decision Records (ADRs)

## Brand

"Sage" = wise agent, knowing helper. Pattern: `sage-<engine>-mcp`. The brand explicitly emphasizes the intelligence/cognition aspect, separating Sage from the execution-only crowd.
