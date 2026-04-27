# ADR-002: Storage Layer

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Sage iki ayrı persistence concern'i taşıyor:

1. **Knowledge graph**: 100K+ node, graph traversal queries (impact analysis, references), incremental update, slot-bazlı izolasyon.
2. **Audit log + slot index**: structured tabular data (transaction history, slot metadata, server registry), ACID, basit queries.

Her ikisi için doğru depolama farklı.

## Kararlar

### 1. Knowledge Graph Storage
**Karar:** KuzuDB (embedded, single-file)
**Alternatifler:** Neo4j (server overhead), DuckDB (analytical, graph-native değil), in-memory only
**Gerekçe:** Native graph database, embedded deployment, columnar storage analytical query'lere uygun, Cypher dialect Unreal reference traversal use case'ine maps. Slot başına ayrı `.kuzu` dosyası → izolasyon kolay. Storage ileride `GraphStore` trait arkasına soyutlanır, gerekirse swap.

### 2. Audit + Index Storage
**Karar:** SQLite
**Alternatifler:** RocksDB (overkill), KuzuDB içine gömülü tablolar (concern karışır), flat file
**Gerekçe:** Mature, transactional, ubiquitous tooling (CLI, GUI, debug); separation of concerns (graph storage farklı access pattern'ine sahip).

### 3. Storage Layout
**Karar:** Slot-bazlı klasör hiyerarşisi:
```
~/.sage-mcp/
├── slots/<slot_id>/
│   ├── manifest.json
│   ├── graph.kuzu/
│   ├── snapshots/
│   └── audit.log
├── index.db  (SQLite, slot index)
└── server.db (SQLite, transport/sessions/registry)
```
**Gerekçe:** Slot izolasyonu net (silmek/taşımak kolay); backup/restore basit; KuzuDB single-file çoklu-dosya sync sorununu önler.

## Sonuçlar

**Olumlu:**
- KuzuDB Cypher Unreal traversal'larında idiomatik
- SQLite zengin tooling (CLI debug, ad-hoc query)
- Slot izolasyonu data corruption blast radius'unu sınırlar

**Olumsuz:**
- KuzuDB community küçük (Neo4j'e göre); bug'a takılırsak `GraphStore` abstraction lazım
- İki ayrı veritabanı = iki ayrı backup discipline
