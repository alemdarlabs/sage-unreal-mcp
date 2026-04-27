# ADR-004: Multi-Editor Support

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Geliştiriciler aynı anda birden fazla UE Editor instance açıyor: host/client multiplayer test, mainline + experimental branch, sample project + ana proje. Server bunları ayırt edebilmeli, tool çağrılarını doğru editor'e route etmeli, paylaşılan kaynakları (knowledge graph, compile binary) yönetmeli.

## Kararlar

### 1. Editor Identity & Labels
**Karar:** Her instance handshake'de `{ id, label?, project_id, path, engine_version, session_id, pid }` sunar.

Label kaynakları (priority):
1. CLI argument: `-MCPLabel=host`
2. Per-instance config: `Saved/Config/.../SageMCP.ini`
3. Server-assigned suffix (`MyProject:1`, `:2`)

**Gerekçe:** Kullanıcı kontrolü açık (CLI/config); fallback davranışı stable.

### 2. Routing Model
**Karar:** Active editor pointer per Claude session + per-tool `_editor` parametresi + ambiguity hatası.
**Alternatifler:** Mandatory `_editor` her call'da (verbose); MCP resource model (`editor://host`).
**Gerekçe:** Smart default tek-instance use case'inin friction'ını siler (yaygın); explicit override multi-targeting için; ambiguity hatası silent wrong-target bug'larını önler.

### 3. Knowledge Graph Sharing
**Karar:** Slot-scoped, instance-scoped değil. Aynı slot'a bağlanan iki instance tek graph paylaşır.
**Alternatifler:** Per-instance graph
**Gerekçe:** Source of truth disk; iki instance da aynı `.uasset`'leri görür. Per-instance graph 2x indexing iş, sync sorunları getirir.

### 4. Concurrent Modification
**Karar:** Optimistic locking via `_expected_version` parametresi (opt-in); session-scoped `verify_before_modify: true` flag'i ile zorunlu.
**Alternatifler:** Pessimistic locking, no concurrency control
**Gerekçe:** Verbosity vs safety arasında dengeli orta. Cross-instance cache invalidation hint'leri AssetRegistry event propagation'ından gelir.

### 5. Shared Module Compile Coordination
**Karar:** Shared module compile tüm etkilenen editor'leri tespit eder, kullanıcıdan onay ister, paralel `save → shutdown → compile → relaunch → reconnect` orkestrasyonu yapar.
**Alternatifler:** Silent compile (PIE'leri kırar), refuse if multi-editor
**Gerekçe:** Tek confirmation step ucuz; silent failure en kötüsü; refusal valid workflows'u bloklar.

## Sonuçlar

**Olumlu:**
- Multi-instance senaryoları natural support
- Knowledge graph duplication önlenir
- Compile/restart orchestration safe by default

**Olumsuz:**
- Active editor mental model kullanıcının takip etmesi gereken state
- Ambiguity hatası ek round-trip Claude için
