# ADR-001: Tech Stack Selection

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

Sage Unreal MCP, Unreal Engine için "intelligence layer" odaklı bir MCP server inşa ediyor. Tasarım kritik trade-off'lara dayanıyor:

- **Plugin tarafı**: Unreal C++ mecbur (engine reflection erişimi).
- **Server tarafı**: Plugin ile aynı dil mi, farklı dil mi? Persistent süreç olarak nasıl çalışacak?
- **Transport**: Claude ↔ Server ve Server ↔ Plugin için ayrı protokoller.
- **Distribution**: Studio'lara dağıtılabilir, single-binary tercih.
- **Performans tavanı**: Knowledge graph 100K+ node; hot path latency önemli.

Persistent server zorunlu çünkü:
1. UE Editor restart sırasında MCP bağlantısı kopmamalı.
2. Knowledge graph state Editor restart'larında yaşamalı.
3. Multi-editor senaryoları tek server'ın çoklu editor yönetmesini gerektirir.

## Kararlar

### 1. Plugin Dili
**Karar:** Unreal C++ (UPlugin formatı)
**Alternatifler:** Python (PythonScriptPlugin)
**Gerekçe:** UCLASS/UPROPERTY/UFUNCTION reflection'a tam erişim, AssetRegistry C++ binding, FScopedTransaction native undo entegrasyonu. Python wrapper'lar yer yer eksik (Slate, custom BP node manipulation). Editor + runtime çalışabilirlik.

### 2. Server Dili
**Karar:** C++23 (modern stack)
**Alternatifler:** TypeScript/Node, Rust, Python, Go
**Gerekçe:** Senior Unreal C++ deneyimi var; tek dil mental model'i context-switching'i ortadan kaldırır. KuzuDB asıl API'si C++; Rust/Go binding'leri wrapper. Performans tavanı en yüksek. In-process embed FFI maliyeti olmadan korunur. TS reddedildi: dependency churn, distribution friction. Rust reddedildi: cxx FFI seam, ikinci dil. Python reddedildi: editor-only, GIL, distribution. Maliyet kabul: manuel MCP impl ~1500 LOC, CMake/vcpkg ergonomi.

### 3. Server Transport (Claude ↔ Server)
**Karar:** HTTP + SSE (Streamable HTTP)
**Alternatifler:** stdio, WebSocket-only, gRPC
**Gerekçe:** stdio server lifetime'ını client lifetime'a bağlar; persistent server için imkansız. HTTP+SSE multi-client native, streaming progress destekler.

### 4. Plugin Transport (Server ↔ Plugin)
**Karar:** WebSocket (localhost)
**Alternatifler:** Named pipes / Unix sockets, gRPC, shared memory
**Gerekçe:** Cross-platform tutarlılık, sub-2ms localhost overhead'i kabul edilebilir, JSON-RPC envelope debug-friendly, gRPC schema rigid. Shared memory engineering complexity disproportionate.

### 5. Build System
**Karar:** CMake + vcpkg (manifest mode)
**Alternatifler:** Meson, Bazel
**Gerekçe:** C++ industry standard, IDE integration olgun, cross-platform tested. vcpkg manifest mode (`vcpkg.json`) reproducible dependency management.

### 6. JSON Library
**Karar:** nlohmann/json (ergonomi) + simdjson (hot path parse)
**Alternatifler:** rapidjson, Boost.JSON
**Gerekçe:** nlohmann ergonomisi en iyi; simdjson MCP message parse'ında 5-10x hızlı.

### 7. Logging
**Karar:** spdlog
**Alternatifler:** Boost.Log, glog
**Gerekçe:** Header-only, structured logging, performans yüksek, ekosistem güçlü.

### 8. Test Framework
**Karar:** Catch2 + ASan + UBSan + TSan
**Alternatifler:** doctest, GoogleTest
**Gerekçe:** Catch2 modern macro syntax, ekosistem geniş. Sanitizer'lar C++ memory model belirsizliklerini compile-time guarantee yokluğunu telafi eder.

## Sonuçlar

**Olumlu:**
- Tek dil stack (plugin + server), shared header/DTO mümkün
- Performans tavanı maksimum
- Native KuzuDB + AssetRegistry erişimi
- In-process embed gelecek seçenek olarak açık
- Single-binary distribution kolay

**Olumsuz:**
- Manuel MCP impl 1-2 hafta yatırım
- CMake/vcpkg ergonomi Cargo'dan geride
- C++ memory model: sanitizer disiplini şart
- Async runtime ekosistemi tokio kadar olgun değil
- İlk geliştirme velocity Rust/TS'ten ~%20 yavaş; uzun vadede telafi
