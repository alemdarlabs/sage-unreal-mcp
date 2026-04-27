# ADR-011: Query DSL (Knowledge Graph Sorgu Mekanizması)

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi (V1)

## Bağlam

Sage'in knowledge graph'ı agent tarafından nasıl sorgulanacak? 3 alternatif değerlendirildi: Cypher subset (KuzuDB native), custom restricted DSL, SQL CTE recursive. Karar: 3-katmanlı API + Cypher subset (Layer 2).

## Kararlar

### 1. 3-Layer API
**Karar:** Tek bir query DSL değil, 3 katmanlı API:
- **Layer 1 (high-level tools)**: `impact_of`, `references_to`, `class_hierarchy`, `find_by_class`, vb. — yaygın sorgular için tek-tool. Token-cheap, sandboxed.
- **Layer 2 (Cypher subset)**: KuzuDB native Cypher'ın read-only alt-kümesi. Esnek, advanced sorgular için.
- **Layer 3 (mutation tools)**: Cypher mutation operatörleri yasak; mutation için ayrı dedicated tool'lar (`modify_actor_property`, `save_assets`, vb.)

**Alternatifler:** Tek katman (sadece Layer 1 veya sadece Cypher).
**Gerekçe:** Tek tool API ya çok kısıtlı (Layer 1 only) ya da çok riskli (Cypher only). 3-layer ergonomik + güvenli orta yol. Layer 1 günlük sorguların %80-90'ını karşılar; Layer 2 edge case'ler için.

### 2. Cypher Subset Boundaries
**Karar:** Read-only subset:
- ✅ Allowed: `MATCH`, `RETURN`, `WHERE`, `WITH`, `ORDER BY`, `LIMIT`, `SKIP`, `OPTIONAL MATCH`, aggregation (COUNT/SUM/AVG/COLLECT), parameters (`$param`), pattern matching
- ❌ Forbidden: `CREATE`, `DELETE`, `SET`, `MERGE`, `REMOVE`, `CALL` (procedures), unbounded path `*`, multi-statement
- Bounded traversal `*1..N` zorunlu — default max `*1..5`, override max `*1..10`

**Alternatifler:** Permissive (mutation da allow), restrictive (Cypher hiç yok).
**Gerekçe:** Mutation Cypher'la = data corruption + audit kaybı; mutation dedicated tool'larla auditable. Unbounded traversal cycle/explosion riski → bounded zorunlu.

### 3. Validator Implementation
**Karar:** KuzuDB'nin kendi Cypher parser'ını reuse et; AST üzerinde whitelist walk yap. Custom mini-parser yazılmayacak.
**Alternatifler:** Custom parser, regex-based filtering.
**Gerekçe:** KuzuDB zaten parse ediyor — reuse zero implementation cost. Regex-based filtering unsafe (escape edilmiş keyword'ler, multi-line query'ler kaçırır).

### 4. Result Token Cap
**Karar:** 8K token hard cap (ADR-007 token optimization ile aligned). Aşılırsa otomatik truncate + `refine_hint` döner. Override: `_no_cap: true` opt-in (advanced use case).
**Alternatifler:** No cap, smaller (2K), larger (32K).
**Gerekçe:** Context budget discipline; 8K = ~50-100 result with metadata, çoğu sorgu için yeterli. `_no_cap` advanced kullanıcı için açık kapı.

## Sonuçlar

**Olumlu:**
- Layer 1 yaygın sorguları minimum token cost'la halleder
- Layer 2 advanced flexibility verir, sandbox'lı
- Cypher industry standard — agent training bias avantajlı
- KuzuDB parser reuse: zero implementation overhead
- Mutation auditable (dedicated tool'lar üzerinden, transaction'lı)

**Olumsuz:**
- Layer 1 vs Layer 2 mental model gerek (agent hangisini kullansın?) — description'larla yönlendirilecek
- Bounded `*1..5` bazı derin transitive closure sorgularını sınırlar (override `*1..10`'a kadar)
- Cypher syntax verbose — Layer 2 sorguları 50-200 token

## Etkilenen Belgeler
- `.claude/docs/api-spec.md` — `query()` tool sözleşmesi detaylanacak
- `.claude/docs/knowledge-graph.md` — query examples eklenecek (V1 implementation sırasında)
