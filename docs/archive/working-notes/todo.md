# Historical Milestone Ledger

> Archive only. This file summarizes the old working ledger that covered Phase 1
> through Phase 4, Milestone 1.5b, and the first dogfooding loop. Use current
> source, tests, `scripts/audit-tools.ps1`, ADRs, README, and AGENTS.md for
> active truth.

## Snapshot

Historical snapshot from 2026-04-30:

- 102 commits.
- 443 plugin handlers.
- 457 server schemas, with 444 editor-aware tools.
- 17 ADRs at the time.
- First 11-gap dogfooding loop completed in one development session.
- Mac-to-Windows production transition started.

Current counts have changed. Run:

```powershell
.\scripts\audit-tools.ps1 -Json
```

## First Dogfooding Loop

A second AI session tested Sage against real Unreal projects and reported 11
gaps in a structured format:

- Target.
- Attempted tool and arguments.
- Result or error.
- Missing capability.
- Suggested fix.
- Workaround.

All 11 gaps were fixed during the same development loop. The useful process
pattern remains: verify the reported gap against source, implement the missing
behavior, run focused validation, then commit.

## Multi-Editor Routing

The first loop exposed that remote tools routed through an implicit active
editor and one code path used the first connected client. ADR-017 fixed this by
adding per-call `_editor` routing with runtime schema injection.

Routing priority:

1. Explicit `_editor`.
2. Active editor pointer.
3. Single editor implicit fallback.
4. Ambiguity error.

## Schema Fixes

The first real MCP client validation found hundreds of invalid schemas caused
by JSON construction pitfalls and required-field shape issues. The fix centered
on using safer helpers for object and array construction, then validating
`tools/list` instead of relying only on direct `tools/call` smoke tests.

## Phase 4 Completion

Phase 4 expanded the Sage tool surface toward UE-MCP parity across:

- Blueprint
- Material
- Asset
- Editor
- Project/source inspection
- Animation
- Niagara
- Gameplay
- PCG
- Landscape
- Foliage
- GAS
- Networking
- Audio
- UMG

Historical completion reported 443 plugin handlers and 454 server schemas.
Current source has moved beyond those numbers.

## Retired Or Completed Follow-Ups

The old ledger tracked:

- Public documentation.
- License and distribution decisions.
- `asset.migrate`.
- MCP transport polish.
- Disconnected `project.get_info`.
- Per-domain smoke tests.

Some of these were implemented, some were superseded, and some changed shape
after ADR-018 and the npm release work. Always verify from source before using
this archive as evidence.
