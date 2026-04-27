# ADR-003: Slot Identity Model

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Multi-editor desteği şu senaryoları cevaplamak zorunda:

- Aynı projenin 2 instance'ı açık (host/client multiplayer test)
- Aynı projenin 2 kopyası farklı path'lerde (backup, git worktree, sample fork)
- Aynı isimli farklı projeler (collision)
- Aynı proje farklı engine version'larda (5.4 + 5.5)
- Proje taşındığında ID stable kalmalı veya migration prompt vermeli

Naif "proje adı" identity collision yapar; "tam path" taşımada kırılır; "ProjectID" klonlamada blind.

## Kararlar

### 1. Slot ID Formula
**Karar:** `slot_id = sha256(project_id || canonical_path || engine_major)`
**Alternatifler:** project_id alone, canonical_path alone, hybrid (project_id + path)
**Gerekçe:** Üç bileşen birleşince tüm gözlemlenen senaryolar doğru çözülür: aynı proje 2 instance → aynı slot; klon → ayrı slot; isim collision → ayrı slot (ProjectID farklı); engine version bump → ayrı slot (reflection metadata uyumsuz).

### 2. Component Sources
**Karar:**
- `project_id`: Unreal'ın `DefaultGame.ini` `[GeneralProjectSettings] ProjectID` GUID'i
- `canonical_path`: `realpath()` + case-normalized + symlink resolved
- `engine_major`: `"5.4"`, `"5.5"` (minor version'ları yok say)

**Gerekçe:** Project ID engine'in kendi GUID'i, stable + unique-by-default. Canonical path platform fragmantation'unu önler (Windows case-insensitive, macOS APFS case-sensitive). Engine major reflection ABI breaks'ini ayırır.

### 3. Duplicate ProjectID Davranışı
**Karar:** Default **separate slots**, opt-in `merge_slots(source, target)` ile birleştir
**Alternatifler:** Default merge
**Gerekçe:** Yanlış merge → data corruption (irreversible); yanlış separate → 2x storage (reversible). Safe by default, explicit by intent.

### 4. Synthetic ID Fallback
**Karar:** ProjectID yoksa kullanıcıya prompt; reddederse `synthetic:sha256(canonical_path + engine_major)`. Synthetic slot flagged.
**Alternatifler:** Sessizce GUID üret ve `.ini`'ye yaz, fail
**Gerekçe:** `.ini` değişimi source control'da görünür, sürpriz olmamalı. Synthetic flag engine_major değişiminde re-index tetikler.

### 5. Project Migration
**Karar:** Path değişiminde otomatik orphan, server detect → "migrate?" prompt
**Alternatifler:** Otomatik migrate, hiçbir şey yapma
**Gerekçe:** Otomatik migrate yanlış pozitif riski (klonlamayı taşıma sanmak). Prompt kullanıcı niyetini doğrular.

## Sonuçlar

**Olumlu:**
- Tüm gözlemlenen multi-editor senaryoları temiz çözülür
- Data isolation by default
- Engine version değişikliklerinde data corruption önlenir

**Olumsuz:**
- 2x storage maliyeti gerçekten paylaşılması gereken klonlarda (`merge_slots` ile çözülür)
- Synthetic ID tracking ek complexity
