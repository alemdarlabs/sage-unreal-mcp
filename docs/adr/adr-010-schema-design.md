# ADR-010: Knowledge Graph Schema Design

**Tarih:** 2026-04-27
**Durum:** Superseded by ADR-018 (V1; bazı özellikler V2 research bekliyor)

## Bağlam

Sage'in knowledge graph'ı UE projesinin yapısal modelini tutar. Schema iskeleti (Asset, Class, Function, Property, Module, Plugin, World, ActorRef) önceden ADR-005'te belirlendi. Bu ADR node/edge attribute'larının detaylarını, BP graph içeriği, function call granularity, soft refs, redirectors gibi konuları finalize ediyor.

## Kararlar

### 1. BP Graph Nodes (K2Node)
**Karar:** Ayrı node tipi `(:K2Node)`, **T3 only** (lazy). İlişki: `(:Function)-[:contains]->(:K2Node)`.
**Alternatifler:** Function node'unda JSON property aggregate.
**Gerekçe:** Queryable olmak için ayrı tip şart; T3 sınırlaması ile storage cost yönetilir.

### 2. Function Call Granularity
**Karar:** Aggregated default — `(:Function)-[:calls {count: int}]->(:Function)`. Per-call-site detay T3 on-demand.
**Alternatifler:** Per-call-site default.
**Gerekçe:** Per-site edge count 10x; aggregated çoğu sorguda yeterli. T3'te per-site available.

### 3. Soft References
**Karar:** Edge property — `(:Asset)-[:depends_on {hard: bool}]->(:Asset)`. Tek edge tipi.
**Alternatifler:** Ayrı edge tipi (`:depends_on_hard`, `:depends_on_soft`).
**Gerekçe:** Schema temizliği; Cypher `WHERE r.hard = true` filtre yeterince okunaklı.

### 4. Redirector Handling
**Karar:** Edge'de `kind` property — `(:Asset)-[:references {kind: "redirector"}]->(:Asset)`. Ayrı node yok.
**Alternatifler:** `(:Redirector)` ayrı node tipi.
**Gerekçe:** Redirector'lar transient state; UE eventually fixup eder. Persistent node overhead'i değer.

### 5. ActorRef Tier
**Karar:** T2 lazy — level open edildiğinde indexlenir. T3 spesifik actor için on-demand.
**Alternatifler:** T1 eager (level scan on connect).
**Gerekçe:** 1000-actor level eager indexing 30s+ alır; ihtiyaç anında daha pratik.

### 6. Property Nesting
**Karar:** T1/T2 flat (top-level UPROPERTY only). T3 nested (full hierarchy `StructA.StructB.Field`).
**Alternatifler:** Always nested, always flat.
**Gerekçe:** Nested representation 5-20x büyür; T3 lazy ile dengelenir.

### 7. Schema Versioning Strategy
**Karar:** Forward-only + auto-detect. `_SchemaVersion` node'u storage'da; server compiled-in version vs storage version karşılaştırır, eksik migration'ları sırayla koşar (`migrations/<NNNN>_<description>.cypher`). KuzuDB transaction içinde; fail → rollback.
**Alternatifler:** Bidirectional migration, manual trigger.
**Gerekçe:** Forward-only basit; backward migration nadiren gerek; transaction safety automatic.

## Open Questions (V2 / Research)

Aşağıdaki konular V1 schema'da temsil edilmiyor. Uygulama geliştirme sırasında araştırılıp ayrı ADR ile karara bağlanacak:

- **BP Exec Pin Routines** — Blueprint'te exec (control flow / beyaz pin) bağlantılarıyla yazılan routine'ler. Mevcut `Function -[:calls]-> Function` edge'i exec flow'u kapsamıyor. Olası çözüm: ayrı edge `(:K2Node)-[:flows_to]->(:K2Node)` (T3, control flow graph). Reflection üzerinden K2Node graph data'sına erişim doğrulanmalı.
- **Collapsed Functions** — BP editor'de "Collapse to Function" ile yaratılan inline function'lar. UFunction reflection'a giriyor mu, "internal" flag'iyle filtre gerekiyor mu, parent function ile ilişkisi nasıl modellenecek? UFunction flag taxonomy'si araştırılmalı.
- **Macro Libraries (`UBlueprintMacroLibrary`)** — Macro'lar function değil; expanded inline. Schema'da asset olarak temsil edilecek mi (içeriği T3'te), yoksa ignore mı? Çağrılan macro'nun expanded body'si nereye kaydedilecek?
- **Event Graphs** — `BeginPlay`, `Tick`, `OnComponentBeginOverlap` gibi event-bound graph'lar `Function` özelleşmesi mi, yoksa ayrı `(:Event)` tipi mi? Event'lerin signature'ı dispatch source'a bağlı.

Bunların V2'ye atılma sebebi: implementasyon başlamadan reflection erişimi belirsiz; "denedik gördük" yapıdan sonra net karar verilebilir.

## Sonuçlar

**Olumlu:**
- T1/T2/T3 tier-aware schema; storage cost yönetilebilir
- Edge property pattern (hard/soft, kind) şemayı sade tutar
- Versioning forward-only basit + transaction-safe
- Open questions explicit listelendi — V1 yarım kalmadı, V2 net hedef var

**Olumsuz:**
- Per-call-site granularity için T3 eager invalidation gerek (changed function → cached calls invalidate)
- Redirector edge property approach UE fixup sırasında graph sync gerektirir
- BP exec/collapsed/macro eksikliği bazı impact analysis sorgularını V1'de sınırlayabilir (özellikle "bu BP routine'i değişirse hangi event etkilenir?" sorgusu)

## Etkilenen Belgeler

- `docs/archive/legacy-graph/knowledge-graph.md` — retired schema spec
- `docs/archive/legacy-graph/database-schema.md` — retired KuzuDB table definitions
- `docs/adr/adr-005-knowledge-graph.md` — bu ADR onun extension'ı; ADR-005 indexing strategy'sini, ADR-010 schema attribute'larını fix eder
