# ADR-011: Query DSL

**Date:** 2026-04-27
**Status:** Superseded by ADR-018

## Context

The original graph layer needed an agent-facing query model. Options included a
Cypher subset, a custom restricted DSL, or SQL recursive CTEs.

## Decision

Use a three-layer query model:

1. High-level tools such as `impact_of`, `references_to`, `class_hierarchy`,
   and `find_by_class`.
2. A read-only subset of KuzuDB Cypher for advanced traversal.
3. Dedicated mutation tools outside the graph query surface.

Mutation Cypher operators would be forbidden. Traversal depth would be bounded
by default.

## Rationale

High-level tools cover common questions cheaply and safely. A restricted Cypher
subset gives advanced users an escape hatch without allowing mutation. Dedicated
mutation tools remain auditable and transaction-aware.

## Consequences

Positive:

- Common graph queries are ergonomic.
- Advanced graph queries remain possible.
- Mutation stays outside ad-hoc graph queries.

Negative:

- Agents must choose between high-level tools and the query layer.
- A safe Cypher subset requires validation.
- Large result sets need strict token caps.

## Supersession

ADR-018 removed graph tools and KuzuDB from the active runtime. Current Sage
does not expose Cypher or graph-backed query tools.
