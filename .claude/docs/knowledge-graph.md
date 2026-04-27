# Knowledge Graph

The "intelligence layer" of Sage: a project-wide structural model the agent queries before acting. See [ADR-005](../decisions/adr-005-knowledge-graph.md) for canonical decisions.

## Why a Graph?

Most engine queries are inherently graph-shaped:
- "What depends on this Blueprint?" → reverse traversal of `depends_on`
- "Show me the class hierarchy of `Character`" → recursive `inherits_from`
- "If I change `WeaponBase`, what breaks?" → multi-hop traversal across `depends_on`, `uses`, `inherits_from`
- "Find all calls to function X" → `uses` edges with type filter

Tabular databases force these into recursive CTEs; graph DBs (Cypher) express them in single-line patterns.

## Schema Skeleton

(Full attribute spec deferred to a future deep-dive document.)

### Nodes

```
Asset(path, package_name, asset_class, mtime, size, t3_indexed_at?)
Class(name, parent_class, flags, module)
Function(name, signature, owner_class, flags)
Property(name, type, owner, flags)
Module(name, plugin?, dependencies)
Plugin(name, version, modules)
World(path, persistent_level)
ActorRef(guid, world, class, transform)  -- optional, lazy
```

### Edges

```
(:Asset)-[:depends_on {hard: bool}]->(:Asset)
(:Class)-[:inherits_from]->(:Class)
(:Class)-[:implements]->(:Class)              -- target is interface
(:Function)-[:uses {kind: "call"}]->(:Function)
(:Function)-[:uses {kind: "read|write"}]->(:Property)
(:Asset)-[:lives_in]->(:Module)
(:Module)-[:lives_in]->(:Plugin)
(:World)-[:contains]->(:ActorRef)
(:Class)-[:contains]->(:Function)
(:Asset)-[:references {kind: "redirector|soft"}]->(:Asset)
```

## Three-Tier Indexing

| Tier | Content | Cost / asset | When |
|---|---|---|---|
| **T1: Manifest** | name, type, path, mtime, size | ~5ms | Eager on connect |
| **T2: Topology** | hard/soft refs, class hierarchy, lives_in | ~10ms (most cached in AssetRegistry) | Eager after T1 |
| **T3: Deep** | full UPROPERTY traversal, BP graph nodes, function bodies | 100-300ms | **Lazy / on-demand** |

For 50K-asset projects:
- T1: ~4 minutes (parallel, 4 threads)
- T2: ~2 minutes (most data from AssetRegistry cache)
- T3: ad-hoc, ~5-15s for batch of 50 assets when queried

## AssetRegistry Leverage

Plugin uses Unreal's own metadata cache directly:

```cpp
IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

TArray<FAssetData> Assets;
Registry.GetAllAssets(Assets, /*bIncludeOnlyOnDiskAssets*/ true);

for (const FAssetData& A : Assets) {
    TArray<FName> Deps;
    Registry.GetDependencies(A.PackageName, Deps);
    // Already cached by engine; we don't reparse .uasset files
}
```

Reimplementing dependency walks would duplicate engine work and miss redirectors, plugin content, EngineConfig overrides.

## Indexing State Machine (per slot)

```
       ┌─────────┐
       │  EMPTY  │ ← new slot, not indexed
       └────┬────┘
            │ plugin connect + bootstrap
            ▼
       ┌──────────────┐
       │ BOOTSTRAPPING│ ← T1 + T2 parallel work
       └────┬─────────┘
            │ T1+T2 complete
            ▼
       ┌──────┐
   ┌──►│ READY │◄─┐
   │   └──┬───┘  │
   │      │ AssetRegistry delta event
   │      ▼      │
   │   ┌────────┐│
   │   │SYNCING │┘ ← incremental
   │   └────────┘
   │
   │  Crash mid-bootstrap:
   │  next start: RESUMING
   │
   │   ┌──────────┐
   │   │ RESUMING │ ← from last KuzuDB-committed batch
   │   └────┬─────┘
   └────────┘
```

## Throttle Controller

Editor responsiveness is a hard requirement. The plugin's worker pool monitors:
- **CPU usage**: capped at ~30% sustained, configurable
- **Editor frame time**: if `GAverageMS` > 16ms, workers `FPlatformProcess::Sleep`

The user must not feel indexing's presence.

## Real-Time Delta

Plugin subscribes to AssetRegistry events:

```cpp
Registry.OnAssetAdded().AddRaw(this, &FSageBridge::OnAssetAdded);
Registry.OnAssetUpdated().AddRaw(this, &FSageBridge::OnAssetUpdated);
Registry.OnAssetRemoved().AddRaw(this, &FSageBridge::OnAssetRemoved);
Registry.OnAssetRenamed().AddRaw(this, &FSageBridge::OnAssetRenamed);
```

Each event becomes a delta message to the server. Throughput target: 50 events/sec real-time; above that, batch.

## Reconnect Strategy

Plugin handshake includes the current AssetRegistry hash:

```json
{
  "type": "hello",
  "asset_registry_hash": "sha256:af3c...",
  ...
}
```

Server compares against last stored:
- **Match**: graph is current; transition to READY immediately.
- **Differ**: request delta listing; reindex only changed assets.
- **Hash absent**: full reindex (first-ever connect to this slot).

## Performance Targets

| Operation | Target | Notes |
|---|---|---|
| T1+T2 cold-start (50K assets) | < 6 minutes | Parallel workers |
| T1 query (single asset metadata) | < 50ms | Hot path |
| T2 query (1-hop dependencies) | < 200ms | KuzuDB indexed |
| T3 query (deep parse on-demand) | < 1s for first hit; cached after | Lazy |
| AssetRegistry delta application | < 100ms | Per event |
| Reconnect with no changes | < 500ms | Hash match |

## Storage Format

KuzuDB single-directory embedded database at `~/.sage-mcp/slots/<slot_id>/graph.kuzu/`. Columnar storage; small footprint per node (~2KB amortized for T1+T2, ~10-50KB if T3-indexed).

For 50K assets:
- T1+T2 only: ~150-200 MB on disk
- With T3 for ~10% of assets: ~300-500 MB

## Open Questions (V2 / Research)

The following structural concerns are deferred to V2 — not represented in V1 schema. See [ADR-010](../decisions/adr-010-schema-design.md) for full reasoning.

- **BP Exec Pin Routines** — control flow (white pin) wiring between K2Nodes; modeling deferred until reflection access is verified
- **Collapsed Functions** — inline functions created via "Collapse to Function" in BP editor; reflection visibility and `UFunction` flag taxonomy under investigation
- **Macro Libraries** — `UBlueprintMacroLibrary` representation undecided (asset-as-T3 vs ignore)
- **Event Graphs** — separate `(:Event)` node type vs `Function` specialization

ADR-005 + ADR-010 are the canonical schema decisions; this section tracks gaps for follow-up.

## See Also

- [Database Schema](database-schema.md) — exact table definitions
- [Architecture](architecture.md) — system context
- [ADR-005](../decisions/adr-005-knowledge-graph.md) — indexing decisions
- [ADR-010](../decisions/adr-010-schema-design.md) — schema design + open questions
