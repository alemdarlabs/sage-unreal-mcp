# ADR-017: Multi-Editor Routing Implementation

**Date:** 2026-04-29
**Status:** Accepted
**Completes:** ADR-004 section 2

## Context

ADR-004 selected active-editor routing plus an optional per-tool `_editor`
parameter. Early dogfooding showed that the implementation still routed remote
tools through a single active pointer and, in one path, through the first
connected WebSocket client.

This broke cross-editor workflows such as reading from one project while
writing to another in the same assistant turn.

## Decision

Add optional `_editor` routing to all editor-scoped remote tools.

Routing order:

1. Explicit `_editor` string, matching session ID, label, or instance ID.
2. Server active editor pointer.
3. Single connected editor implicit fallback.
4. Ambiguity error when multiple editors are connected and no target is
   selected.

## Implementation

| Layer | Change |
|---|---|
| `EditorSession` | Add `ix::WebSocket* ws` for session-to-socket lookup. |
| `BridgeServer::handleHello` | Store the WebSocket pointer during hello. |
| `BridgeServer::dispatchTool` | Add target-aware overload and fix first-client routing. |
| `ToolRegistry::RemoteDispatcher` | Extend signature with target editor. |
| `ToolRegistry::dispatch` | Forward the optional target editor. |
| `MCPServer::onToolsCall` | Extract `_editor`, remove it from plugin args, and pass it to dispatch. |
| `MCPServer::onToolsList` | Inject `_editor` into remote tool schemas at runtime. |
| `main.cpp` | Update dispatcher lambda signature. |

## Schema Injection

Do not manually add `_editor` to every tool schema. Inject it at `tools/list`
time for every `remote == true` tool.

Benefits:

- One source of truth.
- New remote tools automatically gain the parameter.
- Server-only tools such as jobs and health tools do not expose meaningless
  editor routing parameters.

ADR-018 removed graph server-only tools. Remaining server-only tools still omit
`_editor`.

## Rationale

1. DRY schema injection avoids repeating the same parameter across hundreds of
   tools.
2. The change is backward-compatible because `_editor` is optional.
3. It implements ADR-004 without changing plugin handlers.
4. It fixes the old first-client routing bug.
5. It enables parallel cross-editor calls in one assistant turn.

## Rejected Alternatives

- **`with_editor({ session_id, calls })` composition tool**: Moves composition
  into the bridge layer and creates transaction-like semantics outside the MCP
  envelope.
- **Only add `asset.migrate`**: Useful but too narrow; cross-editor read/write
  workflows need general routing.
- **Manually add `_editor` to every tool**: Repetition-prone and easy to break.

## Consequences

Positive:

- All editor-scoped tools support per-call session targeting.
- The old first-client routing bug is fixed.
- Single-editor workflows remain unchanged.
- Ambiguous multi-editor cases return actionable errors.
- Cross-editor parallel tool calls are possible.

Negative:

- `EditorSession.ws` is a raw pointer tied to WebSocket server lifecycle and
  must stay protected by the session mutex.
- `tools/list` responses are slightly larger.
- Plugin handlers do not know the selected editor target because the server
  strips `_editor` before dispatching.

## Verification

Implementation was verified during dogfooding with:

| Scenario | Expected result |
|---|---|
| `get_world` without `_editor` and one editor connected | Uses implicit fallback. |
| `get_world` with an instance ID | Routes to the matching editor. |
| `get_world` with an unknown editor | Returns `EditorNotConnected`. |
| `tools/list` | Shows `_editor` on remote tools only. |
