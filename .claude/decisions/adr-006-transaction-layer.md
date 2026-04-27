# ADR-006: Transaction Layer

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Sage tool'ları Unreal asset/actor mutation'ları yapar. Atomicity, undo desteği, multi-step grouping, conflict detection, audit trail gerekli. Hatalı tool half-applied state bırakırsa debug surface kabusa döner. Kullanıcının manuel `Ctrl+Z` MCP edit'lerini de geri almalı.

## Kararlar

### 1. UTransactor Wrapping
**Karar:** Tool'lar `FScopedTransaction` içinde execute edilir. `Modify()`'lı UObject'ler engine undo stack'ine join eder.
**Alternatifler:** Custom undo stack, no transactions
**Gerekçe:** Native integration → kullanıcının `Ctrl+Z`'si MCP edit'leri reverse eder. Paralel state yönetimi yok.

### 2. Single-op vs Multi-step
**Karar:**
- Single-op (default): her tool call kendi `FScopedTransaction`'ı, atomic
- Multi-step: `begin_transaction(label) / commit / rollback` boundaries; sub-op'lar `_tx` parametresi alır

**Alternatifler:** Time-window grouping, flat-only
**Gerekçe:** Explicit boundaries kullanıcı mental model'ine uyar; "boss arena kur" tek undo step olmalı, 7 değil.

### 3. Optimistic Locking
**Karar:** Optional `_expected_version` parametresi (asset state hash). Session-scoped `verify_before_modify: true` ile zorunlu.
**Alternatifler:** Always required, never offered
**Gerekçe:** Mandatory verbose tek-Claude common case'de; flag strict mode (multi-Claude, manuel + AI concurrent).

### 4. Auto-Rollback on Error
**Karar:** Exception veya validation fail → `Cancel()`. Multi-step transaction'lar atomic by default.
**Alternatifler:** Best-effort partial commits
**Gerekçe:** Half-applied state worst debugging surface. Atomic-by-default safe primitive; opt-in `_atomic: false` narrow case'ler için.

### 5. Bulk Operation Atomicity
**Karar:** Atomic by default, `_atomic: false` opt-in.
**Gerekçe:** Karar 4 ile aynı, bulk shape için.

### 6. Save Discipline
**Karar:** Tool execution dirty bırakır, asla auto-save. Kullanıcı `save_assets` ile commit eder.
**Alternatifler:** Auto-save her modification'da
**Gerekçe:** Kullanıcı final write authority alır; AI accident'le disk'e yazmaz; dirty state `Ctrl+Z` ile temiz revert edilir; save cross-instance cache invalidation propagation point'idir.

### 7. PIE Modification Policy
**Karar:** PIE'de modification hard-rejected default. Opt-in `_allow_pie: true`.
**Alternatifler:** Allow with warning, allow silently
**Gerekçe:** PIE world değişiklikleri transient; testing sırasında "production" data corruption önlenir.

### 8. Revert Semantics
**Karar:** `revert_transaction` compensating transaction yaratır (forward inverse), undo-stack rewrite değil.
**Alternatifler:** Splice into UE undo history
**Gerekçe:** UE'nin linear undo stack'ı ve audit trail integrity korunur. Splicing UE internal invariant violation riski.

## Sonuçlar

**Olumlu:**
- Native UE undo entegrasyonu
- Atomic transaction primitiv'leri
- Concurrent modification için opsiyonel locking
- PIE accidental edit önlenir

**Olumsuz:**
- Multi-step `_tx` parametresi tool API'sini biraz şişirir
- Compensating revert undo stack'te yeni entry yaratır (kullanıcı için ek undo step)
