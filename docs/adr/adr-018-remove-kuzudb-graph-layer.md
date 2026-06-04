# ADR-018: Remove KuzuDB Graph Layer

**Date:** 2026-06-04
**Status:** Accepted
**Supersedes:** ADR-002 knowledge graph storage decision, ADR-005 knowledge graph implementation plan, ADR-011 Cypher query surface, ADR-010 Kuzu-backed schema migration details

## Context

Sage originally carried an embedded KuzuDB graph layer for project indexing, impact analysis, dependency traversal, class hierarchy queries, and an advanced read-only Cypher escape hatch.

During real Windows production work the graph layer became a liability:

1. It added a native prebuilt binary outside vcpkg, with platform-specific fetch scripts and runtime copy/RPATH handling.
2. It forced MSVC CRT and C++ standard workarounds that were unrelated to the rest of the server.
3. It duplicated information that Unreal already exposes through AssetRegistry, reflection, source scanning, and domain-specific tools.
4. It increased maintenance cost while the actual product value was moving toward broad, reliable execution and inspection tools.

## Decision

Remove KuzuDB completely from the active Sage runtime and build graph.

This means:

- No `kuzu::kuzu` imported CMake target.
- No `sage-graph` target.
- No Kuzu fetch scripts.
- No graph source directory in the server.
- No Kuzu smoke/integration tests.
- No `_scan_asset_registry` internal feeder.
- No graph MCP tools: `index_slot`, `index_status`, `impact_of`, `references_to`, `find_unused`, `class_hierarchy`, `query_graph`.
- No AssetRegistry delta stream whose only consumer was the graph layer.

Future project-understanding features must be source-backed and tool-backed first: use live Unreal AssetRegistry, reflection, reference diagnostics, source index/search, and domain tools. A future persistent index is allowed only behind a fresh ADR and must not reintroduce KuzuDB by default.

## Consequences

Positive:

- Server configure/build no longer depends on `third_party/kuzu`.
- Windows runtime no longer needs to load `kuzu_shared.dll`.
- The C++ server can stay uniformly C++23.
- Tool behavior is easier to reason about: live editor state comes from editor tools, not a potentially stale graph snapshot.
- Plugin startup no longer binds AssetRegistry delta hooks only to feed a removed server index.

Negative:

- Historical graph tools are gone; callers must use live reference/reflection/domain tools instead.
- Advanced ad-hoc Cypher queries are gone.
- Some old notes, ADRs, and gap reports remain as history, but this ADR is the active architectural decision.

## Verification Contract

A Kuzu-free build is valid only if all are true:

1. `rg -n "Kuzu|kuzu|sage-graph|kuzu::kuzu|_scan_asset_registry" server plugin tests scripts CMakeLists.txt vcpkg.json` returns no active build/runtime references.
2. `cmake --preset debug` configures without requiring `third_party/kuzu`.
3. `cmake --build --preset debug --target sage-server` succeeds.
4. `cmake --build --preset debug --target sage-tests` succeeds.
5. `ctest --preset debug` succeeds.

