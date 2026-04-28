# ADR-012: MVP Scope — Execution-First, Knowledge-Second

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi ve DOĞRULANDI — Phase 1+2+3+4 tamamlandı (2026-04-28)

## Bağlam

Tasarım sürecinde MVP scope için 3 path önerildi: A (Impact Analysis önce), B (Editor Automation önce), C (Hybrid Minimal). İlk önerim Path A idi — moat'ı sergilemeyi öne çıkardı. Kullanıcı sağlam itirazda bulundu:

> "Çok iyi bir knowledge layer var ama hiçbir yere erişemiyorsun, bir anlamı yok!"

Argüman geçerli. Üç ayrı sebep:

1. **Pratik değer sırası**: AI agent kullanıcıya yardımcı olabilmek için *eyleyebilmeli*. Anlama eyleme hizmet eder, tersi değil.
2. **Kanıt vs iddia**: Knowledge layer kuruldu ama agent execute edemiyorsa "intelligence layer" sadece iddia olarak kalır. Demoda "BP'yi silsem ne kırılır?" diyebilir agent ama "evet sil" diyemiyorsa moat ispatlanmamış.
3. **Bağımlılık doğru yönü**: Knowledge graph zaten execution path'inde gözlem yapıyor (`AssetRegistry::OnAssetAdded/Updated/Removed/Renamed` events). Önce execution kanıtla → events çalışıyor → knowledge layer onların üstüne abone olur. Tersi mantıksız.

## Kararlar

### 1. Sıralama: Execution-First
**Karar:** Phase 1 = Full Execution Layer. Phase 2 = Knowledge Layer (execution üstünde).
**Alternatifler:** Path A (Knowledge first), Path C (Hybrid Minimal).
**Gerekçe:** Pratik değer, kanıt sırası, bağımlılık yönü. Kullanıcı vetosu kabul edildi.

### 2. Phase 1 Kapsamı (4-6 hafta)
**Karar:** Tüm dedicated mutation tools + multi-editor + lifecycle + transactions + compile coordination. Knowledge graph minimum (sadece slot identity + AssetRegistry skeleton sync). Tüm tool'lar UE engine API'lerine direkt erişir, knowledge graph bypass.

Tool kategorileri:
- **Actor**: spawn, delete, modify_property, set_transform, set_visibility, set_tags
- **Component**: add, remove, modify_property, attach, detach
- **Asset**: modify_property, rename, move, duplicate, delete, save_assets, get_dirty_assets, discard_changes
- **Material**: modify_parameter (scalar/vector)
- **Level**: save_level, get_current_level
- **Selection**: select/get/clear actor/asset selection
- **Source control**: auto-checkout via `ISourceControlModule`
- **Editor state**: get_world, get_viewport_state, get_pie_state
- **Transactions**: begin/commit/rollback, bulk_modify, compare_and_set
- **Lifecycle**: list_editors, set_active_editor, slot management (merge/migrate)
- **Compile**: compile_and_reload, analyze_change, get_live_coding_status (per ADR-009)
- **PIE**: run_pie, stop_pie, set_play_mode
- **Tests**: run_tests via Automation Framework

### 3. Phase 2 Kapsamı (4-6 hafta)
**Karar:** Knowledge layer Phase 1'in üstüne kurulur. KuzuDB integration, T1+T2 indexing, AssetRegistry event delta sync, high-level query tools (impact_of, references_to, class_hierarchy), Cypher subset Layer 2.

**Effort:** 4-6 hafta. Phase 1 hooks (event publishing) zaten kuruldu, Phase 2 subscriber'lar takılır → görece hızlı.

### 4. Demo Hedefleri
**Karar:**
- **Demo 1 (end of Phase 1):** "AI projeyi tam kontrol ediyor — actor spawn, asset edit, compile, restart, multi-editor."
- **Demo 2 (end of Phase 2):** "AI projeyi anlıyor + kontrol ediyor — impact analysis, refactor öneri, akıllı eylem. Sage moat ispatlandı."

### 5. Phase Boundary Discipline
**Karar:** Phase 1'de knowledge query tool'ları (`impact_of`, `references_to`, vb.) implement edilmez, ama tool registration framework hazırlanır (Phase 2'de plug-in noktası açık). Aksi halde scope creep ile Phase 1 6 hafta yerine 10 haftaya çıkar.
**Alternatifler:** Phase 1'e high-level query tools'u dahil et.
**Gerekçe:** Knowledge graph olmadan bu tool'ları implement etmek = stub. Stub yazmak yerine event publishing hooks kuralım, Phase 2'de gerçeği takılır.

## Sonuçlar

**Olumlu:**
- Phase 1 sonunda kullanılabilir tool var — commodity dahi olsa pratik değer
- Phase 2 hızlı ilerler (event hooks Phase 1'de hazır)
- Sequential demos: pratik kullanım → moat ispatı; her milestone'da somut output
- Risk dağılımı: Phase 1 düşük teknik risk (UE API, well-trodden), Phase 2 orta risk (KuzuDB, indexing)

**Olumsuz:**
- Phase 1 (4-6 hafta) commodity riski — StraySpark seviyesinde tool, Phase 2 olmadan diferansiye değil
- Total 8-12 hafta MVP — uzun ama gerçekçi
- Phase 1'de "tüm execution" tanımı geniş → scope creep riski yüksek; tight discipline + out-of-scope listesi şart

## Etkilenen Belgeler
- `.claude/docs/mvp-roadmap.md` — Phase 1 + Phase 2 + Phase 4 milestone breakdown (tamamlandı)
- `.claude/docs/api-spec.md` — tool catalog Phase 4 ile genişledi
- `.claude/notes/ue-mcp-tasks.md` — 445/448 coverage audit

## Sonuç (2026-04-28)

Karar **doğru** çıktı. Execution-first sıralaması:
- Phase 1 temel kontrolü verdi → agent eylebildi
- Phase 2 knowledge layer'ı ekledi → agent anladı
- Phase 3 editor restart orchestration'ı kapattı → otonom çalışma
- Phase 4 UE-MCP'nin tüm 445 action'ını kapsadı → tam pariteye ulaşıldı

Endişe konusu olan "Phase 1 commodity riski" gerçekleşmedi; Phase 2 ve Phase 4 tool derinliği ayırt edici oldu.

Toplam: 82 commit · 443 plugin tool · 454 server şema.
