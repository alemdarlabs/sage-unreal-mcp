# Database Schema

Two storage layers: KuzuDB (knowledge graph) and SQLite (audit log + slot index). See [ADR-002](../decisions/adr-002-storage.md) for the layer choice.

## KuzuDB (Knowledge Graph)

One `.kuzu` directory per slot at `~/.sage-mcp/slots/<slot_id>/graph.kuzu/`.

### Node Types (skeleton; deep-dive deferred)

| Node | Purpose | Key Properties |
|---|---|---|
| `Asset` | UE asset (BP, material, mesh, sound, ...) | path, package_name, asset_class, mtime, size, t3_indexed_at? |
| `Class` | UClass / UStruct / UInterface / UEnum | name, parent_class, flags, module |
| `Function` | UFunction (C++ or BP) | name, signature, owner_class, flags |
| `Property` | FProperty (UProperty) | name, type, owner, flags |
| `Module` | Build.cs target | name, plugin?, dependencies |
| `Plugin` | .uplugin | name, version, modules |
| `World` | .umap (level) | path, persistent_level |
| `ActorRef` | Actor instance in level (optional, lazy) | guid, world, class, transform |

### Edge Types

| Edge | Direction | Notes |
|---|---|---|
| `depends_on` | Asset → Asset | Hard or soft reference |
| `inherits_from` | Class → Class | Single inheritance |
| `implements` | Class → Class (interface) | Multiple interfaces possible |
| `uses` | Function → Function/Property | Call or read/write |
| `lives_in` | Asset → Module → Plugin | Containment hierarchy |
| `contains` | World → ActorRef, Class → Function | Owner/owned |
| `references` | Asset → Asset (text-based) | Redirector, soft |

### Indexing State Tables

```cypher
NODE _IndexProgress (
  slot_id      STRING PRIMARY KEY,
  tier         INT,           // 1, 2, 3
  state        STRING,        // bootstrapping, ready, syncing, resuming
  total_assets INT,
  processed    INT,
  last_batch_committed_at TIMESTAMP,
  asset_registry_hash STRING
);

NODE _AssetIndexLog (
  slot_id      STRING,
  package_name STRING,
  content_hash STRING,
  indexed_at   TIMESTAMP,
  tier_max     INT,
  PRIMARY KEY (slot_id, package_name)
);
```

Schema versioning: `_SchemaVersion` node tracks migrations. Breaking changes trigger migration scripts.

## SQLite (Audit + Slot Index)

### `~/.sage-mcp/index.db`

```sql
CREATE TABLE slots (
  slot_id           TEXT PRIMARY KEY,
  project_id        TEXT NOT NULL,
  canonical_path    TEXT NOT NULL,
  engine_major      TEXT NOT NULL,
  display_name      TEXT,
  label             TEXT,
  created_at        INTEGER,
  last_seen_at      INTEGER,
  is_synthetic      INTEGER DEFAULT 0,
  status            TEXT  -- active, orphaned, archived
);

CREATE INDEX ix_slots_project_id ON slots(project_id);
CREATE INDEX ix_slots_canonical_path ON slots(canonical_path);
CREATE INDEX ix_slots_last_seen ON slots(last_seen_at);

CREATE TABLE slot_warnings (
  slot_id    TEXT,
  kind       TEXT,           -- duplicate_project_id, synthetic, ...
  details    TEXT,
  raised_at  INTEGER,
  resolved_at INTEGER
);
```

### `~/.sage-mcp/slots/<slot_id>/audit.log` (SQLite)

```sql
CREATE TABLE transactions (
  tx_id          TEXT PRIMARY KEY,
  parent_tx_id   TEXT,                    -- multi-step parent
  label          TEXT,
  initiated_by   TEXT,                    -- claude session id
  started_at     INTEGER,
  ended_at       INTEGER,
  status         TEXT,                    -- committed, cancelled, errored, partial
  operations     TEXT,                    -- JSON array
  before_hashes  TEXT,                    -- JSON map: object_path → sha256
  after_hashes   TEXT
);

CREATE INDEX ix_tx_started ON transactions(started_at);
CREATE INDEX ix_tx_status ON transactions(status);

CREATE TABLE operation_queue (
  op_id          TEXT PRIMARY KEY,
  tx_id          TEXT,
  tool_name      TEXT,
  args           TEXT,                    -- JSON
  enqueued_at    INTEGER,
  state          TEXT,                    -- pending, dispatched, completed, errored
  result         TEXT                     -- JSON
);

CREATE INDEX ix_opq_state ON operation_queue(state, enqueued_at);
```

### `~/.sage-mcp/server.db`

```sql
CREATE TABLE sessions (
  session_id    TEXT PRIMARY KEY,
  client_kind   TEXT,                     -- claude_code, cursor, cli
  started_at    INTEGER,
  last_seen_at  INTEGER,
  active_editor TEXT
);

CREATE TABLE editor_connections (
  conn_id           TEXT PRIMARY KEY,
  slot_id           TEXT,
  pid               INTEGER,
  connected_at      INTEGER,
  last_heartbeat_at INTEGER,
  state             TEXT                  -- connected, lost, dead, restarting
);
```

## Migrations

Sage schema versioning is forward-only. Each schema version increments a `_SchemaVersion` node (KuzuDB) or row (SQLite). On startup, the server compares stored version to compiled-in version and runs migrations sequentially.

Migration scripts live in `migrations/<version>_<description>.{sql,cypher}`.
