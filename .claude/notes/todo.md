# Phase 1 Active Work

> Reference: `.claude/docs/mvp-roadmap.md`. Last updated: 2026-04-27.

---

## Milestone 1.1 — Server Scaffolding ✓ (commit 3641985)

C++23 server, CMake+vcpkg, MCP layer, HTTP+SSE transport (cpp-httplib), ping
tool, 23/23 tests under ASan+UBSan, end-to-end smoke verified.

ADR-013 (cpp-httplib seçimi) yazıldı.

---

## Milestone 1.2 — Plugin Scaffolding (active)

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

## Açık Sorular / Sonraki

- **Plugin runtime test** — Milestone 1.3 başında host UE projesi içinde sage-server + plugin pair test edilecek (handshake → heartbeat → server log doğrulama).
- **License** → ADR-015 (önceden ADR-014 olarak işaret edildi, ADR-014 Blake3'e gitti). Apache-2.0 / MIT karar vermek lazım.
- **Server-side handshake handler** — şu an MCP server WS değil HTTP+SSE; plugin bridge için ayrı WebSocket transport Milestone 1.5'te kuruluyor (lifecycle + multi-editor). Bu Milestone 1.2'de plugin tarafı tek başına derlenir.
- **Symlink resolution** — `FSageSlotID::ResolveCanonicalPath()` şu an symlink resolve etmiyor. Phase 2 polish.
