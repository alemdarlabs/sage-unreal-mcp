# ADR-008: Brand Identity

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Proje multi-engine bir MCP family'si olacak (Unreal, Unity, Godot). Marka adı:
- AI / agent / cognition çağrışımı taşımalı (intelligence-layer thesis)
- Çakışan büyük marka olmamalı (özellikle dev tooling)
- Tek/iki hece, akılda kalıcı
- Domain bulunabilirliği orta-yüksek
- Multi-engine pattern uygun: `<brand>-<engine>-mcp`

## Kararlar

### 1. Brand Name
**Karar:** Sage
**Alternatifler:** Lumen (UE5 Lumen lighting collision), Lume (Lumière kinship), Phare (FR), Prisme, Nous (Greek "intellect"), Cogito, Janus, Pythia, Pneuma, Sophia (Hanson Robotics overuse), Atlas (Boston Dynamics)
**Gerekçe:** Kullanıcı tarafından seçildi. Sage = "wise agent / knowing helper" connotation Sage'in intelligence-layer thesis'iyle hizalı. Sage Software (accounting) en yakın naming collision ama farklı sektörde; dev tooling alanı boş. Domain `sage.dev` zor ama `sageagent.dev` / `sagemcp.dev` viable fallback.

### 2. Naming Pattern
**Karar:** `sage-<engine>-mcp` (kebab-case, tüm aile için tutarlı)
**Alternatifler:** `<engine>-sage`, `sage_mcp_<engine>`
**Gerekçe:** Engine-first arama-friendly (`sage-unreal-mcp` Google'da yakalanır), kebab-case Unix convention.

### 3. Family Members
**Karar:**
- `sage-unreal-mcp` (active)
- `sage-unity-mcp` (planned)
- `sage-godot-mcp` (planned)

**Alternatifler:** Single multi-engine repo (monorepo)
**Gerekçe:** Her engine farklı build sistemi (UBT, MSBuild/.NET, GDExtension) — monorepo karmaşa. Ayrı repo'lar bağımsız versionlama ve release sağlar.

## Sonuçlar

**Olumlu:**
- Brand intelligence theme ile hizalı
- Pattern her engine için extensible
- Sage Software collision dev tooling'de irrelevant

**Olumsuz:**
- `sage.dev` domain alma zor olabilir (pricey aftermarket)
- "Sage" SEO'da generic terim — `sage mcp` veya `sage agent` ile spesifikleştirme gerek
