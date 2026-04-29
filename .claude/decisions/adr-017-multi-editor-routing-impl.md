# ADR-017: Multi-Editor Routing Implementation (Milestone 1.5b)

**Tarih:** 2026-04-29
**Durum:** Kabul Edildi (ADR-004 §2'yi tamamlar — supersede etmez)

## Bağlam

ADR-004 (Multi-Editor Support) §2'de "active editor pointer + per-tool `_editor` parametresi + ambiguity hatası" kararı verildi. Phase 1.5a'da `list_editors`, `get_active_editor`, `set_active_editor` MCP tool'ları implement edildi; ancak per-call `_editor` parametresi ve `dispatchTool`'un gerçek session-aware routing'i henüz canlıya geçmemişti.

İlk dogfooding loop sırasında (Kale projesinde test eden ikinci Claude) tespit edildi:
1. Tüm 200+ remote tool implicit "active editor" pointer'ına gidiyor; per-call session targeting yok.
2. Aynı assistant turn'ünde A'dan oku + B'ye yaz işlemleri paralel tool call olarak gönderilemiyor (tek pointer, son `set_active_editor` kazanır → race).
3. Bonus mevcut bug: `BridgeServer::dispatchTool` aslında `activeSessionId_`'yi bile kullanmıyordu — `server_->getClients()` setinin ilk öğesini alıyordu (deterministic değil).

Cross-project workflow (FlightProject → Kale uçma component'i kopyalama) ürünün moat değeri — knowledge layer + execution arası köprü — bu tasarım eksiği gerçek bir blocker.

## Karar

**Tüm editor-scoped (remote=true) tool'lara opsiyonel `_editor` parametresi tanı.** Routing sırası:

1. Çağrıda explicit `_editor` (string: session_id, label, veya instance_id) → o session'a route
2. `_editor` yok → server-wide `activeSessionId_` pointer
3. Active da yok → tek editor bağlıysa onu implicit kullan
4. Birden fazla editor + active yok → `EditorNotConnected: "ambiguous target"` hatası

### Implementation Mimarisi

| Katman | Değişiklik |
|---|---|
| `EditorSession` struct | `ix::WebSocket* ws` field eklendi (session_id → ws lookup için) |
| `BridgeServer::handleHello` | Hello sırasında `s.ws = &ws` kaydediliyor |
| `BridgeServer::dispatchTool` | Yeni overload: `(tool, args, timeout, targetIdOrLabel)`. Hedef resolution + sessions map'inden ws lookup. Eski `getClients()[0]` bug'ı fix edildi. |
| `ToolRegistry::RemoteDispatcher` | Signature genişlet: `(tool, args, targetEditor)` |
| `ToolRegistry::dispatch` | Opsiyonel `targetEditor = {}` parametresi, dispatcher'a forward |
| `MCPServer::onToolsCall` | Args'tan `_editor` extract et, args'tan sil, dispatcher'a target olarak geç |
| `MCPServer::onToolsList` | **Runtime schema injection** — her remote tool'a `_editor` opsiyonel property otomatik ekle (DRY: tek noktada, source tool'larda repetition yok) |
| `main.cpp` | `setRemoteDispatcher` lambda'sını yeni signature'a uyarla |

### Schema Injection Tek Nokta

Tüm 200+ tool definition'ına `_editor` field ekleme yerine, `tools/list` response'u oluştururken her `tool.remote == true` için runtime'da injection yapılır. Avantaj:

- Source tool'lar (main.cpp + phase4_schemas.cpp) değişmez — single-point-of-truth
- Yeni eklenen remote tool'lar otomatik kazanır
- Server-side tool'lar (knowledge graph queries, list_editors, ping, vs.) `remote=false` olduğundan otomatik exclude — `_editor` anlamsız olduğu yerde görünmez

12 server-side tool `_editor` almaz: `query_graph`, `impact_of`, `references_to`, `find_unused`, `class_hierarchy`, `index_slot`, `index_status`, `list_editors`, `get_active_editor`, `set_active_editor`, `restart_editor`, `ping`. Diğer 444 tool'a injection uygulanır.

## Gerekçe

1. **DRY**: 200+ tool'a manuel parametre eklemek hem yorucu hem hataya açık (önceki "234 invalid schema" bug'ı bu tarz repetisyondan doğdu). Tek noktada middleware injection sağlam.
2. **Backwards-compatible**: `_editor` opsiyonel; mevcut tek-editor senaryosu hiçbir şekilde değişmez. Eski client'lar kırılmaz.
3. **ADR-004 ile uyum**: Karar 2'de yazılmış olan tasarımı canlıya geçirir, supersede etmez. ADR-004'ün "smart default + per-call override + ambiguity error" üçlüsü tam olarak burada.
4. **Mevcut routing bug fix**: `dispatchTool`'un `getClients()[0]` kullanması zaten broken'dı — multi-editor senaryosunda set_active_editor hiç işe yaramıyordu. Bu commit aynı zamanda o bug'ı kapatır.
5. **Paralel cross-editor execution**: A'dan oku + B'ye yaz aynı assistant turn'ünde paralel MCP tool call olarak gönderilebilir; her biri kendi `_editor`'ünü taşır, server bridge ayrı session'lara route eder.

## Reddedilen Alternatifler

- **B — `with_editor({session_id, calls:[...]})` composition tool**: Tool composition'ı bridge layer'a taşır; sequential listed çağrılar atomik bir grupta. Karmaşık (transaction-like semantics), MCP protocol envelope dışı. A'nın sade middleware injection'ı yeterli.
- **C — Sadece `asset.migrate` tool ekle**: Cross-project asset transfer için pratik ama yetersiz. Component oku A, BP yaz B gibi diğer cross-editor scenario'larda aynı sorun çıkar. C bağımsız bir tool olarak A bittikten sonra eklenecek.
- **Manuel her tool'a `_editor` ekleme**: Source-side repetition; nlohmann brace-init pitfall'ları, tutarsızlık riski (önceki "234 invalid schema" hatasından öğrendiğimiz ders). Reddedildi.

## Sonuçlar

**Olumlu:**
- 444 editor-scoped tool için per-call session targeting devreye girdi
- Mevcut "first client" routing bug'ı fix edildi
- Source tool definition'ları değişmedi (DRY)
- Single-editor senaryosu hiçbir şekilde etkilenmedi (geriye uyumlu)
- Ambiguity error mesajları kullanıcı dostu (`use list_editors`, `pass _editor or call set_active_editor`)
- Cross-editor paralel tool call mümkün hale geldi

**Olumsuz:**
- `EditorSession.ws` raw pointer — ws lifecycle ix::WebSocketServer'a bağlı; close callback'inde sessions map'ten erase ediliyor, ama tasarım gereği "raw pointer + manuel cleanup" güvenliği gerektiriyor. Race açıkken (Close callback ile dispatchTool aynı anda çalışırsa) mutex korumalı.
- `_editor` parametresi tüm 444 tool'da görünür — `tools/list` response boyutunu hafifçe büyütür (~12KB ek; her tool ~30 byte ekstra schema). Token açısından önemsiz.
- Plugin tarafı bilinçsiz: server `_editor`'ı plugin'e iletmeden çıkarıyor, plugin handler'lar değişmeden çalışır. Avantaj (cleaner). Dezavantaj: plugin'in target session'ı bilmesi mümkün değil — şimdilik gerek yok.

## Etkilenen Belgeler ve Dosyalar

| Dosya | Değişiklik |
|---|---|
| `server/src/bridge/editor_session.h` | `ws` field eklendi |
| `server/src/bridge/bridge_server.h` | Yeni `dispatchTool` overload + comment update |
| `server/src/bridge/bridge_server.cpp` | `handleHello` ws kayıt, `dispatchTool` target resolution + ws lookup, eski bug fix |
| `server/src/mcp/tool_registry.h` | `RemoteDispatcher` signature + `dispatch` param |
| `server/src/mcp/tool_registry.cpp` | dispatcher forwarding |
| `server/src/mcp/server.cpp` | `onToolsCall` extract, `onToolsList` injection |
| `server/src/main.cpp` | dispatcher lambda new signature |
| `tests/unit/test_tool_registry.cpp` | dispatcher lambda signatures updated |

## Sonraki Adımlar

1. **`asset.migrate` tool** (Gap C, originally proposed but deferred): UE'nin `FAssetToolsModule::MigratePackages` API'sini sarmalayan, source/dest editör session_id alan tool. Per-call routing artık çalıştığı için bu doğrudan iki editor arasında migration yapabilir.
2. **`_editor` discoverability**: `list_editors` response'unun `tools/list` description'larında daha görünür referansı (örn. "see list_editors for connected editors").
3. **Plugin-level transaction'lar**: Cross-editor transaction (A'dan oku + B'ye yaz atomik bir grupta) için ADR-006 transaction layer'ı genişletilmesi — Phase 5+ konusu.

## Doğrulama

Implementation 2026-04-29 tarihinde Kale projesi (PID 33303) üzerinde test edildi:

| Senaryo | Beklenti | Sonuç |
|---|---|---|
| `get_world` no `_editor` (tek editor) | Implicit fallback → Kale | ✓ DefaultLevel, 108 actor |
| `get_world` `_editor="Kale@dee10b06"` | Instance_id resolution → Kale | ✓ aynı sonuç |
| `get_world` `_editor="NonExistent"` | EditorNotConnected | ✓ -32001 "no editor session matches 'NonExistent' (use list_editors)" |
| `tools/list` schema injection | 444 remote tool'da `_editor` görünür, 12 server-side'da yok | ✓ doğrulandı |
