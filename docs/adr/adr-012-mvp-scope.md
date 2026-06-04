# ADR-012: Execution-First Scope

**Date:** 2026-04-27
**Status:** Accepted and validated

## Context

The early design considered three paths:

- Start with impact analysis and project understanding.
- Start with editor automation and mutation.
- Build a minimal hybrid of both.

The product thesis requires intelligence, but a tool that understands the
project without being able to act is not useful enough for real development
work. The execution layer also produces the observations that later
understanding features can consume.

## Decision

Build the execution layer first, then layer project understanding on top.

Phase 1 must provide real dedicated mutation and inspection tools, lifecycle
controls, transactions, multi-editor support, and compile coordination. It must
not ship read-only stubs that pretend to be production features.

Phase 2 can add deeper project-understanding features once execution is proven.

## Rationale

1. Practical value comes from the agent being able to act.
2. Understanding is only a moat when paired with reliable execution.
3. Execution paths expose the project events and editor state needed for later
   understanding.
4. A stubbed knowledge layer would create false confidence.

## Consequences

Positive:

- The first usable system can perform real Unreal work.
- Tool design is grounded in editor APIs and production workflows.
- Later understanding features can be attached to proven execution surfaces.

Negative:

- Early demos emphasize action more than intelligence.
- Some graph-backed features move later.
- Tool coverage must be broad enough to avoid a toy-feeling execution layer.

## Validation

The decision proved correct during dogfooding. The broad execution surface
exposed real gaps and enabled same-session fixes. ADR-018 later removed the
KuzuDB graph runtime, but the execution-first ordering remains the active
product lesson.
