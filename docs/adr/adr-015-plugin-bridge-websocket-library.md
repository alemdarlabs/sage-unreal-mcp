# ADR-015: Plugin↔Server WebSocket Library — ixwebsocket

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi (ADR-001 plugin bridge transport seçimini supersede eder)

## Bağlam

ADR-001 plugin↔server bridge için **uWebSockets** kararı verdi (high-perf, async, libuv-based). Plugin Scaffolding (Milestone 1.2) sonrası bridge implementation aşamasında seçim yeniden değerlendirildi.

Seçim kriterleri:
- **Kullanım profili**: 1-N (tipik: 1-5) plugin instance, low-frequency tool dispatch (request-response). Yüksek concurrent throughput zorunluluğu yok.
- **Sync vs async**: Tool dispatch protokol semantiği request-response (Claude → server → plugin → server → Claude). `std::promise<ToolResult>` + future pattern sync'e doğru kayıyor.
- **Build complexity**: vcpkg manifest mode'da minimum dep yükü.
- **Cross-platform**: Win64 + Mac (arm64+x64) + Linux first-class destekli.

## Karar

**ixwebsocket** kullan.

### Gerekçe

1. **API ergonomi**: `ix::WebSocketServer` + `setOnClientMessageCallback` pattern'i 30-50 satır impl'e iniyor. uWebSockets'in template-heavy event loop binding'i + uSockets callback dance'ı tool dispatch sync future pattern'ine zıt.
2. **Sync model match**: Sage tool dispatch inherently request-response. `std::promise<ToolResult>` + future await ile temiz, async event loop'un faydası yok bu profilde.
3. **Cross-platform**: Win/macOS/Linux first-class; vcpkg `ixwebsocket` standart paket; OpenSSL/mbedTLS opsiyonel feature (`ixwebsocket[ssl]`).
4. **Dep ağırlığı**: ~3000 LOC, self-contained. uWebSockets uSockets + libuv (transitive) zinciri çeker; binary size ve build time daha yüksek.
5. **Future-proof**: Bottleneck oluşursa transport'ı interface ile abstract edip swap edilir; şimdi over-engineering.
6. **ADR-013 ile tutarlı**: HTTP server için cpp-httplib (header-only, kullanım profiline göre right-size). Aynı disiplin bridge'e de uygulanıyor.

### Reddedilen Alternatifler

- **uWebSockets** (ADR-001 orijinal): Yüksek perf — Sage'in 1-N instance profilinde gereksiz. uSockets/libuv async runtime callback-heavy → sync future pattern ile mismatch. Build/binary footprint daha ağır.
- **Boost.Beast**: Dev overhead büyük; idiomatic API yok; Boost transitive deps ağır.
- **websocketpp**: Aktif değil (~2018'den beri stagnant), modern C++ bağlamında tercih edilmez.

## Server-side Threading Model

- `ix::WebSocketServer::start()` async — kendi accept + per-connection worker thread'lerini açar.
- HTTP+SSE transport (cpp-httplib) `listen()` ana thread'i bloklar.
- Bridge ana thread'den önce `start()` ile başlatılır, HTTP listen sonrası durdurulur.
- Bridge callback'leri ixwebsocket worker thread'lerinde koşar; shared state (session map) `std::mutex` ile korunur.

## Wire Protocol (özet)

JSON over WebSocket. Üst seviye envelope `type` field ile dispatch.

**Plugin → Server:** `hello`, `heartbeat`, `tool_result`, `event`
**Server → Plugin:** `welcome`, `heartbeat_ack`, `tool_call`, `error`

Detay: `server/src/bridge/protocol.h`.

## Sonuçlar

**Olumlu:**
- 30-100 LOC bridge implementation; vcpkg tek dep
- Sync `std::promise<ToolResult>` pattern doğal eşleşme (Milestone 1.3b)
- Cross-platform Windows/macOS/Linux out-of-the-box
- Binary size küçük, build time düşük

**Olumsuz:**
- Yüksek concurrent load'da ixwebsocket thread-per-conn modelinin sınırı (~1k bağlantı). Sage'in 1-10 plugin senaryosunda iz bırakmaz; bottleneck olursa transport interface ile swap edilebilir.
- Phase 1'de plain WS (localhost). Production hardening (TLS, auth) Phase 2'de `ixwebsocket[ssl]` ile aktif edilir.

## Etkilenen Belgeler

- `docs/engineering/tech-stack.md` — "WebSocket: uWebSockets" satırı `ixwebsocket` ile güncellenecek (ADR-015)
- `vcpkg.json` — `ixwebsocket` dependency eklendi
- `server/src/bridge/bridge_server.{h,cpp}` — ixwebsocket-based implementation
- `server/CMakeLists.txt` — `sage-bridge` static lib + `find_package(ixwebsocket)`
