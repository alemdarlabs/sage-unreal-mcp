# ADR-013: HTTP Server Library Seçimi — cpp-httplib

**Tarih:** 2026-04-27
**Durum:** Kabul Edildi

## Bağlam

ADR-001'de C++23 server tech stack'i kabul edildi; HTTP server kütüphanesi "TBD (Crow / cpp-httplib / custom impl)" olarak bırakılmıştı. Phase 1 Milestone 1.1 (Server Scaffolding) HTTP+SSE transport gerektiriyor; somut karar gerekli.

Sage server'ın HTTP transport profil:
- Tek endpoint: `POST /mcp` (Streamable HTTP per MCP spec)
- Body: JSON-RPC 2.0 envelope (request) → JSON response veya SSE chunk stream
- Health probe: `GET /healthz`
- Multi-client (her MCP client kendi session'ı)
- Plugin↔server kanalı **ayrı**: uWebSockets, port 7778 (ADR-001)

## Aday Kütüphaneler

| Kütüphane | License | Header-only | SSE Native | WebSocket | vcpkg | Transitive |
|---|---|---|---|---|---|---|
| cpp-httplib | MIT | Evet | `set_chunked_content_provider` | Hayır | Evet | OpenSSL (opt), zlib (opt) |
| Crow | BSD-3 | Hayır | Manuel chunked | Evet (built-in) | Evet | Boost.Asio + Boost.System |
| Boost.Beast | Boost | Hayır | Düşük-seviye | Evet | Evet | Boost (Asio, System, Optional, ...) |
| Custom impl | — | — | — | — | — | — |

## Karar

**cpp-httplib** seçildi.

### Gerekçe

1. **Use-case match**: HTTP transport tek endpoint + SSE chunked response gerektiriyor. Crow/Beast'in router/middleware/WebSocket altyapısı kullanılmıyor → over-engineered.
2. **WebSocket katmanı bağımsız**: Plugin↔server bridge'i ayrı portta uWebSockets üzerinden (ADR-001). Crow'un built-in WebSocket'ı bu projede redundant.
3. **SSE chunked native**: `Server::set_chunked_content_provider` MCP Streamable HTTP'in tek-endpoint streaming pattern'ına direkt karşılık geliyor. Manuel chunked encoding/SSE framing implementasyonu gerekmez. Phase 1 single-shot JSON ile başlar; streaming tools (compile, indexing) için aynı endpoint chunked'a dönüşür.
4. **Build footprint**: Header-only kütüphane. Crow Boost.Asio + Boost.System transitive bağımlılıklarını çeker (binary-size + build-time cost); Beast tüm Boost ekosistemini gerektirir.
5. **Manuel MCP impl uyumu**: ADR-001 manuel MCP implementation kabul ediyor (~1500 LOC). cpp-httplib'in çıplak callback API'si "transport sadece byte taşır, protokol kendimizinki" ayrımını temiz tutar; Crow'un parameter binding/router katmanı bu ayrımı muğlaklaştırır.
6. **vcpkg manifest desteği**: `cpp-httplib` standart port; özel triplet/overlay gerekmiyor.
7. **Cross-platform**: Windows / macOS / Linux destekli, Sage'in dağıtım hedefleriyle örtüşür.

### Reddedilen Alternatifler

- **Crow**: Built-in WebSocket bizim için kullanılmıyor (uWebSockets ayrı). Boost transitive bağımlılığı build-time + binary-size maliyeti yaratır. Router/parameter binding katmanı `POST /mcp` tekil endpoint için fazla iş.
- **Boost.Beast**: Performans tavanı en yüksek olabilir, ama Sage'in kullanım profili (1-N MCP client, low-RPS protokol katmanı) bu tavana ihtiyaç duymaz. Dev overhead Phase 1 schedule'ını sıkıştırır. İleride bottleneck doğarsa transport `GraphStore`-style trait pattern'le swap edilebilir.
- **Custom impl**: HTTP/1.1 + SSE protocol-level detaylarını yeniden yazmak gereksiz risk. CVE / standart uyumu external lib sorumluluğunda olsun.

## Sonuçlar

**Olumlu:**
- Phase 1 transport ~100 LOC; protokol katmanından temiz ayrılmış
- Phase 2'de SSE chunked streaming için ek kütüphaneye ihtiyaç yok
- Binary size + dependency tree minimal
- macOS/Linux/Windows cross-platform out-of-the-box

**Olumsuz:**
- TLS için OpenSSL ekleme noktası açık kalır (vcpkg üzerinden trivial; gerektiğinde aktivasyon)
- HTTP/2 desteği yok — Phase 1 için kabul edilebilir; gerekirse Phase 3'te Beast'e geçiş yolu açık
- Thread-per-connection modeli yüksek concurrent load'da ölçeklenmez — Sage'in tek/birkaç client senaryosunda iz bırakacak boyutta değil

## Etkilenen Belgeler

- `.claude/docs/tech-stack.md` — "HTTP server | TBD" satırı `cpp-httplib` ile güncellendi
- `vcpkg.json` — `cpp-httplib` dependency eklendi
- `server/src/transport/http_sse_server.{h,cpp}` — cpp-httplib bağımlı transport implementation
- `server/CMakeLists.txt` — `find_package(httplib CONFIG REQUIRED)` ve `httplib::httplib` link
