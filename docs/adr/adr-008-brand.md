# ADR-008: Brand

**Date:** 2026-04-27
**Status:** Accepted

## Context

The product is intended to become a family of game-engine MCP integrations:

- `sage-unreal-mcp`
- `sage-unity-mcp`
- `sage-godot-mcp`

The name must support a broader product family, not only Unreal.

## Decision

Use **Sage** as the product family name.

Use repository and package names in the form:

```text
sage-<engine>-mcp
```

Examples:

- `sage-unreal-mcp`
- `sage-unity-mcp`
- `sage-godot-mcp`

## Rationale

Sage communicates a knowledgeable helper, which fits the product thesis:
execution tools plus project understanding. The name is short, pronounceable,
and engine-neutral.

## Consequences

Positive:

- Extensible across game engines.
- Clear relationship between family and engine-specific implementations.
- Works well in CLI and package names.

Negative:

- "Sage" is generic and has existing use in other industries.
- SEO and discovery need qualifiers such as "Sage MCP" or "Sage Unreal MCP".
