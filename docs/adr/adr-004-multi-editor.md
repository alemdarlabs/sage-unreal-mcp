# ADR-004: Multi-Editor Support

**Date:** 2026-04-27
**Status:** Accepted
**Completed by:** ADR-017

## Context

Unreal developers often run multiple editor instances at the same time: host and
client multiplayer tests, mainline and experimental branches, a sample project
beside the production project, or multiple copies of the same project.

Sage must route tool calls to the correct editor and avoid silent mutation of
the wrong project.

## Decision

### Editor Handshake

Each editor instance registers with:

```json
{
  "id": "...",
  "label": "...",
  "project_id": "...",
  "path": "...",
  "engine_version": "...",
  "session_id": "...",
  "pid": 1234
}
```

Labels come from explicit config first, then project name plus hash, then a
session-derived fallback.

### Routing

Use an active-editor pointer plus optional per-tool `_editor` routing.

Routing order:

1. Explicit `_editor` argument.
2. Active editor pointer.
3. Single connected editor fallback.
4. Ambiguity error when more than one editor is connected and no target is
   specified.

ADR-017 implements this decision.

### Shared State

Shared project state is slot-scoped, not instance-scoped. Two editor instances
for the same slot can share project metadata, while session routing remains
instance-aware.

### Conflict Handling

Use optimistic locking through `_expected_version` where tools need it. A
session-scoped `verify_before_modify` mode can require this check before
mutation.

### Compile Coordination

When a shared module compile affects multiple editors, Sage detects impacted
sessions and coordinates save, shutdown, compile, relaunch, and reconnect only
with explicit user approval.

## Consequences

Positive:

- Single-editor usage remains low-friction.
- Multi-editor usage is explicit and safe.
- Wrong-target mutation becomes an error instead of a silent side effect.

Negative:

- Ambiguous sessions require an extra user or agent decision.
- Compile orchestration is more complex when multiple editors load the same
  module.
