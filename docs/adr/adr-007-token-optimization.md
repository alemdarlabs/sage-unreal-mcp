# ADR-007: Token Optimization

**Date:** 2026-04-27
**Status:** Accepted

## Context

MCP tool responses enter an LLM context window. Verbose responses quickly waste
context budget, especially for Unreal projects with large asset lists,
references, logs, and graph-like relationships.

Token discipline must be designed into the API from the beginning. Retrofitting
it later would create breaking response-shape changes.

## Decision

### Minimal Defaults

Tool schemas default to minimal output. `verbose: true` or a similar opt-in
parameter can expand detail.

### Projection

List and inspection tools should support field projection, such as:

```json
{ "fields": ["name", "type"] }
```

### Pagination

List-returning tools default to bounded results, with cursor or offset support
where appropriate.

### References Before Expansion

Return stable references first. Detailed data should be available through
separate inspect or expand tools.

### Long Strings

Long strings should be truncated with a clear marker and a way to fetch full
content when needed.

### Tiered Data

When a lower-cost tier answers the question, higher-cost data should not be
returned by default.

### Streaming

Long-running tools such as build, indexing, import, or bulk mutation should
stream progress rather than returning one large final blob.

### Hard Caps

Large responses should enforce a practical cap and return a refinement hint
when the query is too broad.

## Consequences

Positive:

- Tool responses stay useful in agent contexts.
- The API encourages precise queries.
- Expensive detail remains available without making it the default.

Negative:

- Tool authors must design response shapes carefully.
- Agents sometimes need a follow-up inspect call.
- Pagination and projection add schema complexity.
