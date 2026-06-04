# ADR-005: Knowledge Graph Indexing

**Date:** 2026-04-27
**Status:** Superseded by ADR-018

## Context

The original Sage thesis included a persistent project graph that would model
assets, classes, modules, references, and impact relationships. Large Unreal
projects can contain tens of thousands of assets, so naive full scans would be
too slow for interactive work.

## Decision

Use a three-tier index:

1. AssetRegistry metadata and dependencies.
2. Reflection data and class relationships.
3. Optional deep Blueprint and graph data.

The plugin would use Unreal AssetRegistry events for incremental updates. The
server would persist indexing state and resume after restart.

## Rationale

The design prioritized:

- Editor responsiveness.
- Incremental freshness.
- Restart recovery.
- Cheap high-level impact queries.
- A foundation for `impact_of`, `references_to`, `find_unused`, and graph
  query tools.

## Consequences

Positive:

- Provided a clear project-understanding roadmap.
- Made impact analysis a first-class product concept.
- Separated high-level tools from low-level graph traversal.

Negative:

- Required a persistent graph runtime and native dependency.
- Duplicated Unreal data that could often be queried live.
- Increased complexity before broad execution tooling was mature.

## Supersession

ADR-018 removed the KuzuDB graph runtime and graph MCP tools. Current Sage uses
live Unreal inspection, reflection, AssetRegistry queries, source search, logs,
and domain-specific diagnostics instead.
