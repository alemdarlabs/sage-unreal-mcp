# ADR-010: Graph Schema Design

**Date:** 2026-04-27
**Status:** Superseded by ADR-018

## Context

The original knowledge graph needed a schema for Unreal assets, classes,
functions, properties, modules, plugins, worlds, actor references, Blueprint
graphs, soft references, and redirectors.

## Decision

Model graph entities explicitly:

- `Asset`
- `Class`
- `Function`
- `Property`
- `Module`
- `Plugin`
- `World`
- `ActorRef`
- `K2Node`

Represent relationships such as ownership, inheritance, dependencies, function
calls, component membership, and soft references as typed edges with stable
attributes.

## Rationale

A typed schema would support impact analysis, reference traversal, unused asset
detection, and advanced query tools without forcing agents to parse raw Unreal
data every time.

## Consequences

Positive:

- Clear graph model for project-understanding tools.
- Better query ergonomics than raw AssetRegistry data.
- Room for Blueprint-level analysis.

Negative:

- Schema evolution would be expensive.
- Deep Blueprint graph indexing has high implementation cost.
- Persistent graph data can become stale or disagree with live editor state.

## Supersession

ADR-018 removed the KuzuDB graph runtime and graph-backed schema from active
Sage. The current system favors live source/editor tools and domain-specific
inspection.
