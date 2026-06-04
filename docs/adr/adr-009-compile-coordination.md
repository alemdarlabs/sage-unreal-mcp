# ADR-009: Compile Coordination

**Date:** 2026-04-27
**Status:** Accepted

## Context

After Sage edits C++ source, it must coordinate with Unreal Editor. Unreal has
several reload paths: Live Coding, deprecated Hot Reload, and full recompile
plus editor restart.

Choosing the wrong path can crash the editor, leave stale reflection metadata,
or force unnecessary restart cycles.

## Decision

### Default Strategy

Use `_strategy: "auto"` by default. The server analyzes changed files:

- `Build.cs`, `.uplugin`, `.Target.cs`: full restart with project file refresh.
- SageBridge plugin changes: full restart.
- Reflection annotation changes such as UCLASS, UPROPERTY, or UFUNCTION: full
  restart.
- Header body-only changes: probe, then escalate if needed.
- `.cpp`-only implementation changes: Live Coding where available.

### Failure Policy

Use `_on_failure: "escalate"` by default. If PIE is active, require explicit
approval before escalation.

### Multi-Editor Policy

If multiple editors load the same module, default to full restart coordination.
Power users may opt into a Live Coding multi-editor path when they accept the
risk.

### Patch Fragmentation

After a configurable number of Live Coding patches, recommend or trigger a full
restart with user approval.

### Bridge Plugin

Changes to `Plugins/SageBridge/` always require full restart. Do not attempt
Live Coding for the bridge plugin itself.

### Reflection Diff

Use a regex/line-based V1 reflection diff. A future version may use libclang
for full AST precision.

## Consequences

Positive:

- Common `.cpp` implementation edits stay fast.
- Reflection and build-system edits take the safer restart path.
- Multi-editor compile behavior is explicit.

Negative:

- V1 reflection detection can miss edge cases.
- Full restarts remain necessary for important classes of changes.
- Multi-editor compile coordination is operationally complex.
