# ADR-009: Compile Coordination Strategy

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Sage'in C++ değişiklikleri sonrası Unreal Editor ile nasıl koordineli compile/reload yapacağı kritik bir mimari soru. UE'nin 3 reload mekanizması var: Live Coding (LC, hot patch), Hot Reload (deprecated), Full Recompile + Restart. Sage Hot Reload kullanmaz; LC ↔ FullRestart yelpazesinde çalışır. Yanlış strateji seçimi → editor crash, stale reflection metadata veya gereksiz 90s restart cycle.

Karar gerektiren noktalar: default strateji, LC fail policy, multi-editor LC davranışı, patch fragmentation handling, bridge plugin self-reload, reflection diff analiz yöntemi.

## Kararlar

### 1. Default Strategy: Auto
**Karar:** `_strategy: "auto"` default. Server `AnalyzeChanges()` ile karar verir (decision tree: Build.cs/.uplugin/.Target.cs → FullRestartWithRegen; Bridge plugin → FullRestart; Reflection annotation değişimi → FullRestart; Body-only header → Probe; Sadece .cpp → LiveCoding).
**Alternatifler:** Mandatory explicit strategy her call'da.
**Gerekçe:** Auto, tool API verbosity'yi minimize eder. Probe + escalate fallback edge case'leri güvenlik ağıyla yakalar.

### 2. LC Failure Policy
**Karar:** Default `_on_failure: "escalate"` (otomatik FullRestart). PIE aktifse server `ask` zorunlu hale getirir.
**Alternatifler:** Default `ask`, default `abort`.
**Gerekçe:** Auto-escalate hızlı; PIE state korunmalı (test runtime data loss önlenir).

### 3. Multi-Editor + LC
**Karar:** Aynı module çoklu editor'de yüklü ise default FullRestart escalation (paralel save → shutdown → compile → relaunch). Power user `_strategy: "live_coding_multi"` ile zorlayabilir.
**Alternatifler:** Default paralel LC her instance'a.
**Gerekçe:** DLL hash mismatch riski sıfır; safer default for inherently advanced scenario.

### 4. Patch Fragmentation
**Karar:** 50 patch sonrası bildirimle otomatik FullRestart. Limit `auto_restart_after_n_patches` config'de tunable.
**Alternatifler:** Sessiz auto-restart, sadece kullanıcı seçimi.
**Gerekçe:** Sessiz auto sürpriz; manual-only fragmentation'ı çözmez. Bildirim + cancel option ortayolu.

### 5. Bridge Plugin LC Kuralı
**Karar:** Sage'in kendi bridge plugin'i (`Plugins/SageBridge/`) değiştiğinde her zaman FullRestart. LC asla denenmez.
**Alternatifler:** LC dene, fail olursa escalate.
**Gerekçe:** Bridge plugin LC = WebSocket koparma + tool registration kayıp + `IAssetRegistry*`/`ITransactor*` cached pointer invalidation. İstisnasız kural.

### 6. Reflection Diff Analiz Yöntemi
**Karar:** V1 regex/line-based (UCLASS, UPROPERTY, UFUNCTION annotation satırları). V2 clang AST (libclang) ile gerçek parse — gelecek versiyon.
**Alternatifler:** Sadece regex (V1'de kalmak), sadece clang AST (V1'den itibaren).
**Gerekçe:** Regex %95 case'i yakalar; clang AST 100% doğru ama libclang dependency ağır (50MB+ binary, build complexity). V1'de probe escalation kalan edge case'leri yakalar (LC fail → FullRestart). V2 enterprise/studio kullanım için.

## Sonuçlar

**Olumlu:**
- Tool API minimal verbose; auto + opsiyonel override pattern
- LC fail durumunda data loss önlenir (PIE-aware policy)
- Bridge plugin self-LC kabusu sistemli olarak önlenir
- Patch fragmentation predictable + tunable

**Olumsuz:**
- Auto kararı yanlışsa probe latency cost (LC dene + fail + restart cycle)
- Multi-editor LC için power-user manual override şart
- V1 regex edge case'lerde gerçek clang AST'a geçiş ileride yatırım gerek

## Etkilenen Belgeler
- `.claude/docs/compile-coordination.md` — bu kararlarla aligned (önceden yazıldı, kararlar onaylandı)
- `.claude/docs/api-spec.md` — `compile_and_reload`, `analyze_change`, `get_live_coding_status` tool sözleşmeleri
