# ADR-005: Knowledge Graph Indexing Strategy

**Tarih:** 2026-04-27
**Durum:** Superseded by ADR-018

> ADR-018 removes the active KuzuDB implementation; this ADR remains historical context only.

## Bağlam

Sage'in intelligence layer'ı projenin yapısal modelini graph'ta tutar. 50K+ asset'lik UE projesinde naif tarama 30-60 dakika sürer. Editor freeze etmemeli, kullanıcı bekleme ekranı görmemeli, restart sonrası baştan başlamamalı, memory bombası olmamalı.

## Kararlar

### 1. Tier-Based Indexing
**Karar:** 3 katmanlı indexleme:
- **T1 (Manifest)**: Asset name, type, path, mtime, size — 5ms/asset, eager
- **T2 (Topology)**: Hard/soft refs, class hierarchy, lives_in — 10ms/asset (AssetRegistry'den çoğu beleş), eager
- **T3 (Deep)**: UPROPERTY traversal, BP graph, function bodies — 100-300ms/asset, **lazy/on-demand**

**Alternatifler:** Single-pass full deep index, fully lazy
**Gerekçe:** Tam deep parse 30-60 dk cold-start (kullanılamaz); tam lazy "what depends on X" sorgu'larında multi-second. T1+T2 sorguların >%90'ını <6 dk'da karşılar; T3 point-of-use'a deferred.

### 2. AssetRegistry Leverage
**Karar:** Plugin `IAssetRegistry::GetAllAssets()` ve `::GetDependencies()` kullanır. Custom `.uasset` parser yazılmaz.
**Gerekçe:** AssetRegistry engine'in cached source'u; reimplementation duplication, ayrıca redirector / plugin content / EngineConfig override edge case'lerini kaçırır.

### 3. Throttle Controller
**Karar:** Plugin worker pool CPU usage (~%30 cap) ve editor frame time (>16ms → pause) bazlı throttle.
**Alternatifler:** Fixed thread count, no throttle
**Gerekçe:** Editor responsiveness hard requirement; kullanıcı indexing'in varlığını hissetmemeli.

### 4. Resumable Indexing
**Karar:** Indexing state KuzuDB'de persisted; restart son committed batch'ten devam.
**Alternatifler:** Restart-from-scratch
**Gerekçe:** 50K-asset projeleri her server restart'ta full re-index kabul edilemez. AssetRegistry hash check delta-only sync sağlar.

### 5. Real-Time Delta
**Karar:** Plugin `OnAssetAdded/Updated/Removed/Renamed` event'lerine subscribe; event'ler server'a stream edilir, graph incremental güncellenir.
**Alternatifler:** Polling, on-demand re-index
**Gerekçe:** Sub-second freshness live editing için; engine'in kendi notification path'iyle uyumlu.

### 6. Reconnect Strategy
**Karar:** Plugin handshake'de AssetRegistry hash sunar; server kayıtlı hash ile karşılaştırır. Match → ready immediately. Differ → delta-only sync. Full reindex sadece fallback.
**Alternatifler:** Her reconnect'te full reindex
**Gerekçe:** Reconnect'ler sık (Editor restart sonrası); full reindex performance kabusu.

## Sonuçlar

**Olumlu:**
- Cold-start <6 dk (50K asset için T1+T2)
- Editor frame time bütçesi korunur
- Restart resilience: state on disk, delta sync
- Real-time freshness sub-second

**Olumsuz:**
- T3 lazy parse ilk query'de 100-300ms latency
- Tier-awareness her tool'da uygulanmalı (discipline)
- AssetRegistry hash hesaplama ek overhead (kabul edilebilir)
