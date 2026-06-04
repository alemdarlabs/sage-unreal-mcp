# ADR-014: Slot ID Hash — Blake3 over SHA-256

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi (ADR-003 §1 hash algoritma seçimini supersede eder; bileşen seçimi değişmedi)

## Bağlam

ADR-003 slot_id formülasyonunu `sha256(project_id || canonical_path || engine_major)` olarak tanımladı. Plugin scaffolding (Milestone 1.2) sırasında UE 5.7 source taraması yapıldı:

- `Engine/Source/Runtime/Core/Public/Hash/`: **Blake3**, BuzHash, CityHash, Fnv, xxhash
- `Engine/Source/Runtime/Core/Public/Misc/SecureHash.h`: SHA-1, MD5
- **SHA-256 built-in olarak yok**

Seçenekler:
1. Public domain SHA-256 implementation embed (~200 LOC, audit + maintenance burden)
2. External dep (vcpkg `picosha2`, OpenSSL)
3. UE built-in cryptographic hash kullan: **Blake3**

## Karar

**Blake3 kullan.** Slot identity için kriptografik güç yeterli, UE built-in olduğu için plugin tarafı external dep'siz.

### Gerekçe

1. **UE built-in**: `Hash/Blake3.h` (`FBlake3`, `FBlake3Hash`) UE 5.x'te core modülünde mevcut. Plugin tarafı için sıfır external dependency, sıfır maintenance.
2. **Cryptographic strength**: Blake3 modern cryptographic hash; 256-bit output, pre-image + collision resistance SHA-256 ile karşılaştırılabilir, BLAKE2 + Bao tabanlı, public review. Slot identity için fazlasıyla güçlü.
3. **Performans**: Blake3 SHA-256'dan ~5-10x daha hızlı (single-threaded), SIMD-accelerated, parallelizable. Slot identity computation nadir ama hesaplama maliyeti gözle görülür şekilde düşer.
4. **Server-side parity**: vcpkg `blake3` paketi mevcut; Phase 2'de server slot validasyonu gerekirse aynı algoritma minimal cost ile eklenir.
5. **Embed yapmama gerekçesi**: "Public domain SHA-256 satır" gerçekte sıfır maintenance değil — endianness, edge case'ler, platform-specific SIMD tuning gerek. Built-in kullanmak idiomatic.

### Slot ID Formula (ADR-003 §1 revizyonu)

```
slot_id = blake3(project_id || \x00 || canonical_path || \x00 || engine_major)
```

- `\x00` (null byte) bileşen separator → bileşen sınırı disambig (gelecekte bir bileşen `||` içerse çakışma olmasın)
- 256-bit (32 byte) output → 64 char lowercase hex string
- Input encoding: UTF-8 (`FTCHARToUTF8` UE plugin tarafında)

ADR-003'ün diğer kararları (component sources, duplicate ProjectID davranışı, synthetic fallback, project migration) bu kararla etkilenmedi.

## Sonuçlar

**Olumlu:**
- Plugin tarafı sıfır external dep
- Server tarafı (Phase 2) vcpkg üzerinden minimal cost ile parite sağlar
- Daha hızlı identity hesaplama (low-impact ama free win)
- UE built-in: ASan/UBSan altında valide edilmiş code

**Olumsuz:**
- "Blake3" SHA-256 kadar yaygın değil; paydaşlar identification'ı açıklarken kısa not gerekir
- ADR-003 metni güncellenmeli (supersede notu)

## Etkilenen Belgeler

- `docs/adr/adr-003-identity-model.md` — §1 başına supersede notu eklenecek
- `plugin/Source/SageBridge/Private/Identity/SageSlotID.cpp` — Blake3 ile compute (Milestone 1.2)
- (Phase 2) `server/src/identity/slot_id.cpp` — vcpkg `blake3` paketi ile

## Alternatifler Reddedilme Nedenleri

- **SHA-256 (public domain embed)**: 200 LOC + test gereksinimi + endian ele alma. Built-in mevcutsa embed gereksiz NIH.
- **SHA-256 (external dep, e.g. picosha2)**: Plugin tarafı için ek dep zinciri (Build.cs ThirdParty integration). Built-in tercih edilir.
- **OpenSSL EVP_sha256**: UE OpenSSL ile geliyor ama plugin tarafı OpenSSL header expose etmek için ekstra Build.cs uyarlaması; Blake3 daha temiz.
- **SHA-1**: Cryptographic broken (collision saldırıları); identity için yeterli olsa da future-proof değil. Reject.
