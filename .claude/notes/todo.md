# Phase 1 Active Work

> Reference: `.claude/docs/mvp-roadmap.md`. Last updated: 2026-04-27.

---

## Milestone 1.1 — Server Scaffolding ✓ (commit 3641985)

C++23 server, CMake+vcpkg, MCP layer, HTTP+SSE transport (cpp-httplib), ping
tool, 23/23 tests under ASan+UBSan, end-to-end smoke verified.

ADR-013 (cpp-httplib seçimi) yazıldı.

---

## Milestone 1.2 — Plugin Scaffolding ✓ (commit ffc223d)

> Goal: UE plugin connects to server, sends handshake, maintains heartbeat,
> reconnects with backoff. No tool dispatch yet (Milestone 1.3).

### Cross-platform foundation
- [x] `.gitattributes` — line ending normalization
- [x] `BUILD.md` — toolchain + build commands (macOS / Linux / Windows)
- [x] `SAGE_UE_ROOT` env var convention (BUILD.md + scripts)
- [x] `scripts/build-plugin.sh` (macOS / Linux)
- [x] `scripts/build-plugin.ps1` (Windows)

### A. UPlugin
- [x] `plugin/SageBridge.uplugin` (Type=Editor, LoadingPhase=PostEngineInit, PlatformAllowList=[Win64,Mac,Linux])

### B. Module build
- [x] `plugin/Source/SageBridge/SageBridge.Build.cs` — Core, CoreUObject, Engine, DeveloperSettings, WebSockets (Public); UnrealEd, EditorSubsystem, Json, JsonUtilities, Projects (Private)

### C. Module + Log
- [x] `Public/SageBridge.h` — `LogSageBridge` log category + module decl
- [x] `Private/SageBridgeModule.cpp` — `IMPLEMENT_MODULE`

### D. Settings (UDeveloperSettings)
- [x] `Public/SageBridgeSettings.h` — ServerUrl, EditorLabel, reconnect/heartbeat tunables, bAutoConnect
- [x] `Private/SageBridgeSettings.cpp` — CLI override (`-SageMCPLabel=`)

### E. Identity
- [x] `Public/Identity/SageSlotID.h`
- [x] `Private/Identity/SageSlotID.cpp` — Blake3 (ADR-014); resolves project_id, canonical path, engine major
- [x] `Public/Identity/SageEditorIdentity.h` — handshake schema struct
- [x] `Private/Identity/SageEditorIdentity.cpp` — handshake JSON builder (api-spec.md §Handshake)

### F. WebSocket client
- [x] `Public/Connection/SageWebSocketClient.h` — `TSharedFromThis`, multicast delegates, FConfig
- [x] `Private/Connection/SageWebSocketClient.cpp` — connect → handshake → heartbeat loop, exponential backoff reconnect, FTSTicker driven

### G. Editor subsystem
- [x] `Public/SageBridgeSubsystem.h` — `UEditorSubsystem`, exposed to Blueprints
- [x] `Private/SageBridgeSubsystem.cpp` — owns client lifecycle, OnConnected → SendHandshake

### H. Verification ✓
- [x] `scripts/build-plugin.sh` → UAT BuildPlugin (UE 5.7.4, Mac universal: arm64 + x64) — **35 saniye, sıfır error**
- [x] 30/30 compile + link adımı temiz; `UnrealEditor-SageBridge.dylib` lipo'd
- [x] `build/plugin/Binaries/Mac/UnrealEditor-SageBridge.dylib` packaged (universal)
- [x] `build/plugin/SageBridge.uplugin` + Source/ + Intermediate/ + FilterPluginMac.ini emitted
- [ ] Deferred runtime test (Milestone 1.3 entry) — host UE projesi içinde plugin yükle, handshake log gözlemle

### I. Decisions
- [x] ADR-014 — Slot ID hash: Blake3 over SHA-256 (UE built-in, performans, sıfır external dep)
- [x] ADR-003 §1 — Hash algorithm satırı ADR-014'e revize notu eklendi

### J. Commit
- [x] Commit (build verified) — see git log

---

---

## Milestone 1.3a — Plugin↔Server Bridge ✓ (active commit pending)

> Goal: Server tarafı WebSocket bridge endpoint'i, hello/welcome handshake,
> heartbeat round-trip, session bookkeeping. Tool dispatch (1.3b) sonraki adım.

### A. Library decision
- [x] ADR-015 — ixwebsocket (replaces uWebSockets ADR-001 plugin bridge seçimi)
- [x] tech-stack.md — WebSocket satırı `ixwebsocket (ADR-015)`

### B. Server-side bridge
- [x] `server/src/bridge/protocol.{h,cpp}` — wire protocol (hello, heartbeat, tool_result, event ↔ welcome, heartbeat_ack, tool_call, error), `parseType`, `parseHello`, `parseHeartbeat`, `parseToolResult`, factory functions
- [x] `server/src/bridge/editor_session.h` — connected editor state
- [x] `server/src/bridge/bridge_server.{h,cpp}` — ix::WebSocketServer adapter, session map (mutex), handshake + heartbeat handlers
- [x] `server/CMakeLists.txt` — `sage-bridge` STATIC target, ixwebsocket::ixwebsocket link
- [x] root `CMakeLists.txt` + `vcpkg.json` — ixwebsocket dep
- [x] `server/src/main.cpp` — bridge wire-up, signal-aware shutdown, env vars (`SAGE_WS_HOST`, `SAGE_WS_PORT`)

### C. Tests
- [x] `tests/unit/test_bridge_protocol.cpp` — 8 test (parseType, parseHello happy/sad, parseHeartbeat, parseToolResult, factories)
- [x] `tests/integration/bridge_smoke.cpp` — standalone executable, ixwebsocket client, sends hello + heartbeat, asserts welcome + heartbeat_ack
- [x] `tests/CMakeLists.txt` — duplicate-library warning fix (sage-mcp transitive via sage-bridge)

### D. Verification ✓
- [x] vcpkg ixwebsocket[core,sectransp,ssl] install — 8 saniye
- [x] cmake configure 9.3 s; build 11/11 clean (after dup-fix)
- [x] ctest 31/31 PASSED (23 önceki + 8 bridge protocol)
- [x] Smoke `sage-bridge-smoke` → connect + hello → welcome + heartbeat → heartbeat_ack → normal close (1000) — ASan/UBSan clean
- [x] Server log doğrulama: `Bridge listening on ws://127.0.0.1:7778/bridge`, handshake detail (`slot=smoke-slot, label='smoke', engine=5.7.4`), graceful shutdown

### E. Commit
- [ ] `feat: phase 1 milestone 1.3a — plugin↔server WebSocket bridge`

---

## Milestone 1.3b — Tool Dispatch (pending)

> Goal: Server'a `tools/call` geldiğinde plugin'e RPC route et, `std::promise<ToolResult>` ile sync future await; plugin tarafı `ToolDispatch` ile FScopedTransaction içinde çalıştırıp sonuç gönderir.

- [ ] Server: pending RPC table (`std::unordered_map<TxId, std::promise<ToolResult>>`)
- [ ] MCPServer integration: registry tool tipi "remote" → bridge route
- [ ] Plugin: `ToolDispatch` module (gelen tool_call → registered handler dispatch)
- [ ] Tool registration mechanism plugin-side
- [ ] First tool: `spawn_actor` (Milestone 1.3c için altyapı)

---

## Milestone 1.3c — First Mutation Tool: spawn_actor (pending)

- [ ] Plugin handler: `UEditorActorSubsystem::SpawnActorFromClass`, FScopedTransaction wrapping
- [ ] Server tool registration: `spawn_actor` schema + remote route
- [ ] End-to-end test: Claude → server → plugin → spawn → response

---

## Açık Sorular / Sonraki

- **Real UE host project test** — Mac üzerinde sage-server'ı çalıştırıp gerçek bir UE 5.7 projesinde plugin'i yükleyip handshake doğrulaması (kullanıcı talep etti, Milestone 1.3a sonrası test penceresi)
- **License** → ADR-016 (Apache-2.0 önerim)
- **Symlink resolution** — `FSageSlotID::ResolveCanonicalPath()` Phase 2 polish
- **Plugin tarafı incoming message parse** — şu an `OnMessageReceived` raw string, parse Milestone 1.3b'de eklenir
