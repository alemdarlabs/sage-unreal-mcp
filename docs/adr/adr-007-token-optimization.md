# ADR-007: Token Optimization Principles

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

MCP tool response'ları LLM context'ine girer. Mevcut MCP server'ların çoğu verbose response döner; bu Claude'un context budget'ını hızla şişirir. Sage'in intelligence layer'ı potansiyel olarak büyük graph data döndürür (impact analysis, references); discipline yoksa hızla 10K+ token'lık tek response'lar olur. Tasarım anında değil retrofit ile çözülürse API stability bozulur.

## Kararlar

### 1. Schema-on-Demand
**Karar:** Tool schema default minimal; `verbose: true` opt-in genişletme.
**Gerekçe:** Default %80 use case için sufficient; advanced parametreler ileri kullanıcılar için açık.

### 2. Field Selection
**Karar:** GraphQL-tarzı `fields: ["name", "type"]` parametresi response shape'i daraltır.
**Gerekçe:** Hangi field'a ihtiyaç olduğu Claude'a malum; gereksiz veri talep etmesin.

### 3. Pagination + Cursor
**Karar:** List-returning tool'lar default limit 50, cursor tabanlı next page.
**Gerekçe:** Sınırsız liste yasak; Claude büyük result set'leri stream şeklinde işleyebilir.

### 4. ID-First Responses
**Karar:** Reference (path / numeric ID) döndür, detail için ayrı `expand(ids)` veya `inspect` tool.
**Gerekçe:** Çoğu sorgu identifier yeter; full object payload ihtiyaç anında.

### 5. Smart Truncation
**Karar:** Long string'ler `…` ile kesilir, `show_full(ref)` tam halini verir.
**Gerekçe:** Function body, asset description gibi alanlar binlerce karakter; Claude hangi parçayı göreceğini seçsin.

### 6. Tier-Aware Queries
**Karar:** Knowledge graph query'leri T1 yetiyorsa T2/T3 data dönmez. Query parameter'da min/max tier set edilebilir.
**Gerekçe:** T3 data ~10x büyük; gereksiz tier'a inilmesin.

### 7. Streaming
**Karar:** Long-running tool output (compile, indexing, bulk) SSE chunk'larıyla stream.
**Gerekçe:** Claude progress'i real-time görür, geç gelen response'larla blocked değil.

### 8. Hard Cap
**Karar:** Response > ~8K token otomatik truncate + `refine_query` öneri.
**Gerekçe:** Tek response context'in %1'inden fazlasını yememeli; limit forcing function.

### 9. Compact Encoding
**Karar:** Asset path'leri stable numeric ID'lere map'le; query'lerde ID kullan.
**Gerekçe:** Path string'leri ortalama 60 karakter; ID 4-byte. 100 asset listesi 6KB → 400 byte.

### 10. Query DSL
**Karar:** Cypher-like query subset; sadece istenen traversal döner, surrounding subgraph değil. Dialect seçimi (KuzuDB native Cypher, custom restricted, SQL CTE) deferred.
**Gerekçe:** Implicit "all related data" antipattern; agent explicit traversal pattern belirtsin.

## Sonuçlar

**Olumlu:**
- Tool API'leri context-budget conscious doğar
- LLM context bloat antipattern'ı baştan engellenir
- Pagination/streaming ergonomik

**Olumsuz:**
- Tool design discipline gerek (her tool'da bu prensipler uygulanmalı)
- Bazı operasyonlar 2-3 round-trip (list → expand) tek response yerine
