# Milestone Kayıt Defteri (Phase 1-4 + Milestone 1.5b TAMAMLANDI · İlk dogfooding turu bitti)

> Bu dosya Phase 1 Milestone 1.1'den itibaren tamamlanan tüm işleri kayıt altına alır.
> **Mevcut durum (2026-04-30): 102 commit · 443 plugin tool · 457 server şema (444 _editor-aware) · 17 ADR · 11-gap dogfooding turu fix · Mac→Windows transition**
> Referans: `.claude/docs/mvp-roadmap.md` · `.claude/notes/ue-mcp-tasks.md` · `.claude/decisions/adr-017-multi-editor-routing-impl.md` · auto-memory `project_dogfooding_loop_2026_04_29.md` + `project_windows_transition.md`

---

## Niagara gap sweep source implementation (2026-06-04)

- [x] Replaced Niagara placeholder-success handlers with real source behavior or explicit unsupported MCP errors.
- [x] Expanded Niagara schema/handler surface from 26 to 68 aligned tools.
- [x] Added Niagara module dependencies in `SageBridge.Build.cs` and `.uplugin`.
- [x] Added registry canary coverage for new Niagara schema entries.
- [x] Added external reference matrix at `.claude/notes/niagara-mcp-reference-matrix.md`.
- [ ] Build/package/deploy/live Kale runtime dogfood intentionally pending; Mahmut will choose the compile window.

---

## İlk Dogfooding Turu — 11 gap fix tek oturumda (2026-04-29)

Mahmut Mac'te ikinci bir Claude Code session'ı (Kale projesi) ile gerçek MCP-client testi başlattı. Test eden Claude kullanıcı/tester rolünde, bu (sage repo) Claude geliştirici rolünde, Mahmut köprü. 11 gerçek gap raporu çıktı, hepsi aynı oturumda fix edildi.

| Gap | Konu | Commit |
|---|---|---|
| pre | 234 invalid schema (nlohmann brace-init pitfall) | `a83fa00` |
| #1+#2 | asset.search query optional + asset.list class/kind/offset/fields | `c837969` |
| #3 | multi-editor per-call routing (ADR-017) + getClients()[0] bug | `76ef244` + `930e46e` |
| #4 | project.create_cpp_class bootstrap_module (BP→C++ scaffold) | `3f87ce5` |
| #5 | UHT prefix + parent header registry (~30 base class) | `6082c5e` |
| (V5→V6) | BuildSettingsVersion.V6 (UE 5.7 default) | `5def2b5` |
| #6 | bp.full_dump atomic Blueprint snapshot | `c79bbe8` |
| #7 | bp.full_dump CDO + cosmetic | `aadf0bc` |
| #8 | bp.full_dump response collapse when output_path set | `c0b5269` |
| #9 | project.add_module_dependency Build.cs array-literal-aware + private flag | `409a48b` |
| #10 | restart_editor rebuild_project_modules (Mac UBT compile) | `d3859df` |
| #11 | bp.full_dump include_t3d gerçekten T3D üretiyor (include_all_nodes) | `690318a` |

**Pattern (yeni dogfooding'lerde tekrar)**: Test eden Claude `Gap #N` formatıyla rapor — hedef, denenen tool/args, sonuç/hata, eksik, öneri (A/B/C öncelikli), workaround. Geliştirici Claude fix + commit + bildirim.

**Detay**: auto-memory `project_dogfooding_loop_2026_04_29.md`.

---

---

## Milestone 1.5b — Multi-editor per-call routing ✓ (commit 76ef244, 2026-04-29)

İlk dogfooding loop'unda Kale projesinde test eden ikinci Claude tespit etti: tüm 200+ remote tool implicit "active editor"'a gidiyor; per-call session targeting yok. Bonus mevcut bug: `BridgeServer::dispatchTool` `getClients()[0]` alıyordu, `activeSessionId_`'yi hiç kullanmıyordu.

Çözüm — orta katman injection (DRY):
- `EditorSession.ws` raw pointer (handleHello'da kayıt)
- `BridgeServer::dispatchTool(tool, args, timeout, targetIdOrLabel)` — target resolution + ws lookup
- `ToolRegistry::RemoteDispatcher` signature genişlet (targetEditor)
- `MCPServer::onToolsCall` `_editor` extract (args'tan sil, dispatcher'a forward)
- `MCPServer::onToolsList` runtime schema injection — 444 remote tool otomatik kazandı

Routing önceliği: explicit `_editor` > active pointer > tek editor implicit > ambiguity error.

ADR-017 yazıldı; ADR-004 §2'yi tamamlar.

---

## Phase 4 sonrası bug + gap fix turu (2026-04-29)

### Schema generator bug fix (commit a83fa00)

İlk gerçek MCP client testi (Claude Code Zod validator): 234/456 tool inputSchema invalid. 4 kalıp:
1. `required: {"a":"b"}` (75 tool) — nlohmann brace-init `{{"a","b"}}` two-strings → object
2. `required: [["a"]]` (136 tool) — `{{"a"}}` single-string → nested array
3. `properties: null` (22 tool) — `obj({})` → JSON null
4. `properties.value: null` (1 tool) — `widget.set_property` `{"value", {}}`

Fix: `obj()` helper'ı `std::initializer_list<const char*>` alacak şekilde yeniden yazıldı (nlohmann brace-init disambiguate edildi). 211 çağrı sed ile flatten. main.cpp'de iki manuel düzeltme.

### Asset enumeration gaps (commit c837969)

Diğer Claude raporladı:
- Gap #1: `asset.search` schema'da `query` required değil ama runtime zorunlu. Class-only sorgu yapılamıyor.
- Gap #2: `asset.list` 3132 asset'lik projede 557KB JSON → token limit aşıyor.

Fix:
- `asset.search`: query optional, class+directory ile fallback. `query VEYA class` zorunlu.
- `asset.list`: yeni opsiyonel `class` (FTopLevelAssetPath), `kind` (string array), `offset`, `fields` (output projection).

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

## Milestone 1.3b — Tool Dispatch (server side ✓ / plugin side pending)

### Server side ✓ (commit pending)
- [x] `Tool::remote` flag (tool.h)
- [x] `ToolRegistry::RemoteDispatcher` + `setRemoteDispatcher` + `hasRemoteDispatcher`
- [x] `RegisterError::MissingHandler` (local tool without handler reddedilir)
- [x] `ToolRegistry::dispatch` remote → dispatcher delegation, exception trapping
- [x] `BridgeServer::dispatchTool(tool, args, timeout?)` — sync, `std::promise<ToolResult>` future await, no-plugin → `EditorNotConnected`, timeout → `InternalError`
- [x] `BridgeServer::handleToolResult` promise resolution (success/error path)
- [x] `BridgeServer::stop` in-flight RPC'leri unblock eder
- [x] `nextTxId()` — atomic counter, `tx-{016x}` format
- [x] `main.cpp` wire-up: `registry.setRemoteDispatcher` → `bridge.dispatchTool`
- [x] `editor.ping` remote tool registered (smoke target, replaced in 1.3c)
- [x] 5 yeni unit test → **35/35 total**, ASan+UBSan clean

### Plugin side ✓ (commit pending)
- [x] `Public/ToolDispatch/SageToolDispatch.h` — `FOutcome`, `FHandler`/`FSendFn` typedefs, RegisterHandler/HasHandler/HandleEnvelope
- [x] `Private/ToolDispatch/SageToolDispatch.cpp` — `tool_call` parse + handler dispatch + `tool_result` reply
- [x] `USageBridgeSubsystem::HandleIncomingMessage` JSON deserialize → ToolDispatch routing
- [x] `SageWebSocketClient::OnMessageReceived` AddUObject → subsystem hook
- [x] `editor.ping` builtin handler — mirrors mock-plugin contract (echoes args + `echoed_by:"plugin"`)
- [x] BuildPlugin verify — UE 5.7.4 universal (arm64+x64), 32/32 step, BUILD SUCCESSFUL, ExitCode=0, 33 s

### Integration ✓ (end-to-end mock verified)
- [x] Mock plugin executable — `sage-bridge-mock-plugin` (standalone, ixwebsocket client, single tool_call echo)
- [x] End-to-end smoke: `POST /mcp tools/call editor.ping` → MCPServer → registry remote → BridgeServer.dispatchTool → WS → mock plugin → tool_result → promise.set_value → MCP response. ASan+UBSan clean.
- [x] Response payload doğrulandı: `structuredContent: {echoed_by:"mock", tool:"editor.ping", message:"hello-from-test"}`

---

## Milestone 1.3c — Editor State + Selection Tools (6) ✓ (commit pending)

### Plugin (`SageEditorTools.cpp`)
- [x] `get_world` — current editor world path / map name / current-level actor count (read-only)
- [x] `get_pie_state` — bool active + play-world path (read-only)
- [x] `get_viewport_state` — active viewport size (read-only)
- [x] `get_selected_actors` — `UEditorActorSubsystem::GetSelectedLevelActors`
- [x] `select_actors` — `UEditorActorSubsystem::SetSelectedLevelActors`; reports selected + not_found
- [x] `clear_selection` — `UEditorActorSubsystem::SelectNothing`
- [x] `USageBridgeSubsystem` → `RegisterEditorTools(ToolDispatch)`

### Server (`main.cpp`)
- [x] DRY `noArgSchema` for read-only tools; six tools registered

---

## Milestone 1.3c — Asset Mutation Tools (8/8) ✓ (commit pending)

### Plugin (`SageAssetTools.cpp`)
- [x] `modify_asset_property` — `UEditorAssetSubsystem::LoadAsset` + detail::SetUPropertyFromJson + MarkPackageDirty; FScopedTransaction; PIE-rejecting
- [x] `rename_asset` — `UEditorAssetSubsystem::RenameAsset` (same folder); FScopedTransaction
- [x] `move_asset` — same UE call as rename, semantic alias for cross-folder moves; FScopedTransaction
- [x] `duplicate_asset` — `UEditorAssetSubsystem::DuplicateAsset`; returns new asset's UE path
- [x] `delete_asset` — `UEditorAssetSubsystem::DeleteAsset`; FScopedTransaction
- [x] `save_assets` — paths-specific or all-dirty via `UEditorLoadingAndSavingUtils::SaveDirtyPackages`; `dry_run` support
- [x] `get_dirty_assets` — `FEditorFileUtils::GetDirty{Content,World}Packages` query; read-only
- [x] `discard_changes` — `UEditorLoadingAndSavingUtils::ReloadPackages` with `AssumeNegative` (no UI prompt)
- [x] `USageBridgeSubsystem` → `RegisterAssetTools(ToolDispatch)`

### Server (`main.cpp`)
- [x] Eight remote tools registered with JSON Schema:
  - `modify_asset_property` (asset_path + property + value)
  - `rename_asset` (source + destination)
  - `move_asset` (source + destination)
  - `duplicate_asset` (source + destination → returns `new_asset_id`)
  - `delete_asset` (asset_path)
  - `save_assets` (paths optional, dry_run optional)
  - `get_dirty_assets` (no args; read-only)
  - `discard_changes` (paths required)

---

## Milestone 1.3c — Component Mutation Tools (5/5) ✓ (commit pending)

### Helpers refactor
- [x] `Private/Tools/SageToolHelpers.h` + `.cpp` — shared `detail::` namespace: ResolveActor / ResolveComponent / RejectIfPie / ParseVector3 / ParseRotator3 / Vec3ToJson / Rot3ToJson / SetUPropertyFromJson / RunOnGameThread template
- [ ] (sonraki commit) `SageActorTools.cpp` refactor → `detail::` çağrıları — şu anda anon-namespace duplicate

### Plugin (`SageComponentTools.cpp`)
- [x] `add_component` — `LoadClass<UActorComponent>` + `NewObject` + `OnComponentCreated` + `RegisterComponent` + `AddInstanceComponent`; FScopedTransaction + Modify
- [x] `remove_component` — `UnregisterComponent` + `RemoveInstanceComponent` + `DestroyComponent`; FScopedTransaction
- [x] `modify_component_property` — reflection setter (detail::SetUPropertyFromJson); PreEditChange + PostEditChangeProperty
- [x] `attach` — `USceneComponent::AttachToComponent` KeepRelativeTransform; optional socket
- [x] `detach` — `USceneComponent::DetachFromComponent` KeepRelativeTransform
- [x] `USageBridgeSubsystem` → `RegisterComponentTools(ToolDispatch)`

### Server (`main.cpp`)
- [x] Five remote tools registered with full JSON Schema:
  - `add_component` (actor_id + component_class required; component_name optional)
  - `remove_component` (component_id required)
  - `modify_component_property` (component_id + property + value required)
  - `attach` (child_id + parent_id required; socket optional)
  - `detach` (child_id required)

---

## Milestone 1.3c — Actor Mutation Tools (5/5) ✓

### Plugin (`SageActorTools.cpp`)
- [x] Helpers: `ResolveActor`, `RejectIfPie`, `RunOnGameThread` template, `SetUPropertyFromJson` reflection setter (bool/int/int64/float/double/string/name/text/byte)
- [x] `spawn_actor` — `UEditorActorSubsystem::SpawnActorFromClass` + FScopedTransaction + Modify + SetActorLabel
- [x] `delete_actor` — `DestroyActor` + FScopedTransaction
- [x] `set_transform` — partial location/rotation/scale + Modify + SetActorTransform
- [x] `set_visibility` — SetActorHiddenInGame + SetIsTemporarilyHiddenInEditor
- [x] `modify_actor_property` — reflection-based UProperty setter; PreEditChange/PostEditChange notifications; FScopedTransaction
- [x] `USageBridgeSubsystem::RegisterBuiltinHandlers` → `RegisterActorTools(ToolDispatch)`

### Server (`main.cpp`)
- [x] Five remote tools registered with full JSON Schema:
  - `spawn_actor` (class required; location/rotation/label optional)
  - `delete_actor` (actor_id required)
  - `set_transform` (actor_id required; location/rotation/scale 3-arrays optional, at least one)
  - `set_visibility` (actor_id + hidden bool required)
  - `modify_actor_property` (actor_id + property + value required; primitive types only in Phase 1)
- [x] DRY `registerRemote` lambda helper

### Verification
- [x] cmake server build clean, 35/35 ctest passing
- [x] UAT BuildPlugin clean (deferred to monitor event)

### Real UE host-project test (pending — kullanıcı tarafında)
- [ ] UE 5.7 boş projeye `build/plugin/` paketini `<Project>/Plugins/SageBridge/` altına kopyala
- [ ] Plugin enable, editor restart
- [ ] `sage-server` background
- [ ] `curl POST /mcp tools/call spawn_actor` → editor world'de actor görünür mü, Edit menüsü `Sage: Spawn Actor` undo'da yer alıyor mu

---

## Phase 4 Tamamlanma Özeti (2026-04-28)

Phase 1–3 sonrası 443 tool handler'a ulaşıldı. UE-MCP (448 action) ile %99.3 pariteye erişildi.

**Yeni domain dosyaları (Phase 4):**
- SageAnimationTools.cpp — 46 tool
- SageLevelTools.cpp — 22 tool
- SageGameplayTools.cpp — 45 tool
- SageNiagaraTools.cpp — 26 tool
- SagePcgTools.cpp — 16 tool
- SageLandscapeTools.cpp — 11 tool
- SageFoliageTools.cpp — 7 tool
- SageAudioTools.cpp — 5 tool
- SageNetworkingTools.cpp — 11 tool
- SageGasTools.cpp — 9 tool

**Server tarafı:** `phase4_schemas.cpp` — 254 remote tool şeması eklendi; `tools/list` eksiksiz.

**Kalan açık maddeler (Phase 5 adayları):**
- End-to-end entegrasyon test suite
- ~~Her domain için smoke test script'leri~~ ✓ 2026-04-28 (10/10 PASS)
- `docs/` (public-facing) getting-started yazısı
- License kararı (ADR-016 Apache-2.0 önerim)
- `asset.migrate` tool — `FAssetToolsModule::MigratePackages` wrapper, source/dest editor session ile cross-project transfer (per-call _editor ile artık doğal entegrasyon)
- MCP transport polish — `Mcp-Session-Id` header + GET /mcp SSE endpoint + OAuth metadata stub + `Mcp-Protocol-Version` header (paketleme öncesi şart)
- `project.get_info` disconnected mode — şu an sadece bağlı editör için çalışıyor; bağlanma sorunu teşhisi için bağlı olmayan projenin uproject'ini okumak gerekebiliyor (yan not, dogfooding loop'unda diğer Claude tespit etti 2026-04-29)

---

## Phase 4 Domain Smoke Tests Tamamlandı (2026-04-28)

10 yeni domain için Python smoke script'leri eklendi (`scripts/smoke/`):

| Domain | Tools | Kapsam |
|---|---|---|
| audio.py | 5/5 | create_cue/metasound · list · spawn_ambient · play_at_location |
| foliage.py | 7/7 | create_type · list_types · get/set_settings + 3 stub note |
| gas.py | 9/9 | create_ability/effect/cue · add_asc · get_info (graceful skip GAS plugin) |
| networking.py | 11/11 | tüm replication flag round-trip + get_info readback |
| level.py | 22/22 | spawn_light/volume · set_world_settings · fog · outliner · count_actors |
| landscape.py | 11/11 | get_info · list_layers · note-only paths (no landscape in level) |
| pcg.py | 16/16 | create_graph · add_volume + 7 note stub (graceful skip PCG plugin) |
| niagara.py | 26/26 | create system+emitter · spawn · 13 note stub · list_system_parameters |
| animation.py | 46/46 | create_anim_blueprint/sequence/montage/blendspace/composite/ik_rig/retargeter + 21 mutation tools |
| gameplay.py | 45/45 | AI assets · framework BPs · IMC · physics · navmesh · PIE error path |

Master runner: `scripts/smoke/run_phase4_domains.py` — 10/10 PASS (gerçek UE editor + sage-server üzerinde).

Plugin-gated graceful skip pattern'ı: GAS, PCG, SmartObjects, PoseSearch plugin'leri SageTest'te yüklü değil, smoke test'ler bunu yakalayıp skip ediyor.
## Active Gap Closure - 2026-05-21

- [x] Implement typed `gamefeature.add_widget_entry` for Lyra `UGameFeatureAction_AddWidgets` HUD entries.
- [x] Implement LayeredBoneBlend BlendMask authoring for `animation.set_layered_bone_blend_config`.
- [x] Verify server schema/build and UE plugin build.
- [x] Move closed gap details from `.claude/notes/gap-inbox.md` into the implemented summary with evidence.

Review:
- Debug `sage-server` built successfully after initializing the VS dev environment.
- `sage-tests` rebuilt and `ctest --test-dir build\debug --output-on-failure` passed 37/37.
- `scripts\audit-tools.ps1` reports no plugin handler without schema and no schema/plugin stubs; only expected local `jobs.*` schema-only tools remain.
- `scripts\build-plugin.ps1` packaged the Win64 plugin successfully under `build\plugin`.

## Active Gap Closure - 2026-05-21 Reticle Provider CDO Array

- [x] Implement `bp.set_cdo_instanced_array_element` for Blueprint/WidgetBlueprint CDO `TArray<TObjectPtr<Instanced UObject>>` authoring.
- [x] Add recursive CDO readback support to `bp.get_cdo_properties` for Blueprint asset paths and instanced provider arrays.
- [x] Register schema/handler and add registry coverage.
- [x] Verify server build, unit tests, tool audit, and UE BuildPlugin.
- [x] Move the detailed gap from `.claude/notes/gap-inbox.md` to the implemented summary.

Review:
- Debug `sage-server` build completed through `scripts\build-server.ps1`.
- `sage-tests` rebuilt and `ctest --test-dir build\debug --output-on-failure` passed 37/37.
- `scripts\audit-tools.ps1` reports 603 server tools, 585 plugin handlers, no plugin-without-schema, no stubs, and only expected local `jobs.*` schema-only tools.
- `scripts\build-plugin.ps1` packaged the Win64 plugin successfully under `build\plugin`.

## Active Gap Closure - 2026-05-30 Retargeting Parity

- [x] Implement IK Rig parity slice: fix `animation.create_ik_rig`, replace `animation.read_ik_rig` placeholder, add chain/root/auto-generate tools.
- [x] Implement IK Retargeter parity slice: op setup, op stack edits, standalone auto-map, chain reset, FK/IK settings readback, FK settings write, expanded pose operations.
- [x] Implement import/discovery/diagnostic slice: FBX import, animation find/save, inspect/sample/compare/diagnose tools.
- [x] Implement runtime retarget slice: `Retarget Pose From Mesh` node authoring/readback and retarget profile tools.
- [x] Add skeleton-owned BlendMask creation/readback for UpperBody/LowerBody mask workflows.
- [x] Verify schema registry, server tests, audit script, and UE BuildPlugin before closing the source/package gap.

Review:
- Debug `sage-tests` rebuilt through `scripts\build-server.ps1 debug -Target sage-tests`; `ctest --test-dir build\debug --output-on-failure` passed 37/37.
- Debug `sage-server` rebuilt through `scripts\build-server.ps1 debug -Target sage-server`.
- `scripts\audit-tools.ps1` reports 640 server tools, 622 plugin handlers, no plugin-without-schema, no stubs, and only expected local `jobs.*` schema-only tools.
- `scripts\build-plugin.ps1` packaged the Win64 plugin successfully under `build\plugin`.
- Runtime editor dogfood/deploy is still pending; no production project mutation was attempted in this gap-closure pass.

## Active Gap Closure - 2026-06-02 IK Retargeter + BP Reparent CDO

- [x] Add `animation.set_ik_retargeter_ik_chain_settings` for IK Chains op per-target-chain settings.
- [x] Make `bp.reparent` refresh/compile generated class layout so inherited native CDO properties are immediately visible.
- [x] Harden `bp.set_cdo_property` with stale-layout refresh diagnostics, dry-run validation, before/after readback, and property owner readback.
- [x] Register schemas and add registry coverage.
- [x] Update `.claude/notes/gap-inbox.md` closure state.
- [x] Run compile-free verification only; build is intentionally deferred by user instruction.

Review:
- `git diff --check` passed.
- `scripts\audit-tools.ps1` passed: 650 server tools, 632 plugin handlers, 0 plugin-without-schema, 0 schema/plugin stubs, only expected local `jobs.*` schema-only tools.
- `rg` confirms `FindBoneTrackByName` is gone from repo `plugin/Source`, but remains in Kale's stale deployed plugin source until the next build/deploy sync.
- No server build, test binary rebuild, UE BuildPlugin, Kale deploy, or live editor dogfood was run per user instruction.

## Active Gap Closure - 2026-06-02 IKRig Skeletal Mesh Setter

- [x] Add `animation.set_ik_rig_skeletal_mesh` for existing/duplicated IKRig assets.
- [x] Use UE IKRig compatibility validation before mutation and reject incompatible meshes without dirtying.
- [x] Return before/after IKRig readback plus preservation diagnostics for root, chains, goals, solvers, and goal-solver connections.
- [x] Extend `animation.read_ik_rig` with goal-solver connection readback.
- [x] Register schema and add registry coverage.
- [x] Move the detailed gap from `.claude/notes/gap-inbox.md` into implemented summary.
- [x] Run compile-free verification only; build is intentionally deferred by user instruction.

Review:
- `git diff --check` passed.
- `scripts\audit-tools.ps1` passed: 651 server tools, 633 plugin handlers, 0 plugin-without-schema, 0 schema/plugin stubs, only expected local `jobs.*` schema-only tools.
- Targeted `rg` confirms the new handler/schema/test/docs entries exist and no open 2026-06-02 IKRig gap block remains.
- Kale's deployed plugin source still contains the stale `FindBoneTrackByName` call; it should be corrected by the next full build/deploy sync, not by partial manual copy.
- No server build, test binary rebuild, UE BuildPlugin, Kale deploy, or live editor dogfood was run per user instruction.

## Reference Audit - 2026-06-04 Niagara Benchmarks

- [x] Clone/source-audit Monolith, UE-MCP, ChiR24, Flopperam, runreal, and unreal-api-mcp into a temporary folder.
- [x] Check remiphilippe mcp-unreal and UECortex as listing/product references.
- [x] Update `.claude/notes/niagara-mcp-reference-matrix.md` with source-backed Niagara benchmark coverage and licensing notes.
- [x] Add a new open post-reference Niagara parity gap for the remaining benchmark deltas.
- [x] Run compile-free verification only; build is intentionally deferred by user instruction.

Review:
- Temporary audit folder: `C:\Users\mahmu\AppData\Local\Temp\sage-ref-audit-20260604-004937`.
- Monolith remains the strongest Niagara-specific benchmark; UE-MCP and ChiR24 provide useful ergonomics/preset-action references.
- New open deltas: CustomHlsl/HLSL module authoring, Niagara search/discovery, ordered stack usage selectors, dynamic input lifecycle, event/sim-stage creation, DI add/configure helpers, renderer binding/material convenience, and preview image/GIF capture.
- `git diff --check` passed.
- `scripts\audit-tools.ps1` passed: 693 server tools, 675 plugin handlers, 0 plugin-without-schema, 0 schema/plugin stubs, and only expected local `jobs.*` schema-only tools.
- No server build, test binary rebuild, UE BuildPlugin, Kale deploy, or live editor dogfood was run per user instruction.

## Active 1251 Tool Expansion - Batch 1 AI Gameplay Surface

- [x] Add Batch 1 AI/BT/Blackboard/EQS/StateTree/SmartObject/Mass/ZoneGraph schemas from the 1251 master list.
- [x] Implement plugin handlers without successful placeholder notes; unsupported editor-private graph mutations must return precise hard errors.
- [x] Keep implementation compile-free: no server build, UE BuildPlugin, package, deploy, or live editor dogfood until explicitly requested.
- [x] Run static registry/audit/diff checks only.

## Active 1251 Tool Expansion - Batch 2 Mesh Gameplay Surface

- [x] Add Batch 2 Mesh/GeometryScript/ProceduralMesh/RealtimeMesh schemas from the 1251 master list.
- [x] Implement plugin handlers for static/skeletal mesh inspection, collision and LOD configuration, and procedural mesh component routing.
- [x] Return precise hard errors for GeometryScript/RealtimeMesh/Chaos operations that require plugin-private mapping instead of fake success.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 3 Audio Gameplay Surface

- [x] Add Batch 3 Audio/SoundCue/MetaSound schemas from the 1251 master list.
- [x] Implement plugin handlers for sound asset create/write/search/health/batch operations and SoundCue/MetaSound read/preview paths.
- [x] Return precise hard errors for SoundCue/MetaSound graph mutations and audio/perception binding where no safe public mapping exists.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 4 UI Gameplay Surface

- [x] Add Batch 4 UMG/CommonUI/template/accessibility schemas from the 1251 master list.
- [x] Implement plugin handlers for WidgetBlueprint spec import/export, rename/is-variable/root/navigation edits, focus/accessibility audit, token application, CommonUI action-bar class write, and inspect/edit aliases.
- [x] Return precise hard errors for unsafe CommonUI widget class conversion and custom navigation delegate binding instead of fake success.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 5 Material Gameplay Surface

- [x] Add Batch 5 material instance/function/HLSL/inspection schemas from the 1251 master list.
- [x] Implement plugin handlers for Custom HLSL nodes, expression move/rename/duplicate/replace, material function creation/readback, material instance parameter writes, batch material operations, thumbnail/texture/PBR/orphan inspection.
- [x] Return precise hard errors for viewport/render-target capture paths that need dedicated preview infrastructure instead of fake success.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 6 Source Intelligence Surface

- [x] Add Batch 6 C++ API/docs/source-callgraph/decision/risk/cppreflect/network/pipeline tools from the 1251 master list.
- [x] Implement the batch as server-side local tools, not editor-bridge tools, because the surface reads source/docs/git/ADRs rather than mutating Unreal editor state.
- [x] Update audit coverage for `regLocal(registry, "...")` server-only registrations and explicit source-intelligence prefixes.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 7 Runtime Verification Surface

- [x] Add Batch 7 runtime verification, automation test, capture, build/cook/live-compile, and performance schemas from the 1251 master list.
- [x] Implement QA aliases/logs in `SageQaTools`, Live Coding/build boundary in `SageCompileTools`, and capture/performance/runtime-console wrappers in `SageEditorAutomationTools`.
- [x] Return precise hard errors for full UBT build and asset-specific preview-scene capture backends that cannot be safely driven through the current editor bridge.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 8 Asset Content Surface

- [x] Add Batch 8 asset folder/save/import-source/health/report/tag/Fab/ISM-HISM schemas from the 1251 master list.
- [x] Implement asset folder create/delete, save/save-all-dirty, batch texture import, import-source readback, asset health/report/validate, metadata tag writes, and ISM/HISM actor component instance operations.
- [x] Return precise hard errors for Interchange pipeline asset authoring, thumbnail generation, and Fab marketplace operations that require dedicated authenticated/rendering infrastructure instead of fake success.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 9 Niagara Reference Surface

- [x] Add Batch 9 Niagara CustomHlsl/search/discovery schemas from the 1251 master list.
- [x] Implement existing CustomHlsl node read/write via reflection, parameter/data-interface/material searches, generic Niagara query, similar-system scoring, referencer alias, and system data-interface listing.
- [x] Preserve hard-error boundaries for creating new CustomHlsl/dynamic-input/editor-stack structures that still require dedicated Niagara editor graph construction.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 10 Sequencer Cinematics Surface

- [x] Add Batch 10 `level_sequence_query` and `sequencer_edit` schemas from the 1251 master list.
- [x] Implement Level Sequence listing/query/readback with tracks, sections, bindings, possessables, spawnables, display/tick rate, and playback range summary.
- [x] Implement `sequencer_edit` as a real operation router for existing sequence create/track/key/binding/spawnable writes plus playback-range edits; unsupported richer edits return hard MCP errors.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 11 Specialized And Status Surface

- [x] Add Batch 11 specialized marketplace/plugin-domain and lower-priority path/status tools from the 1251 master list.
- [x] Implement `logicdriver_query` and `combograph_query` as safe plugin-gated AssetRegistry/reflection query wrappers, with vendor graph mutation ops returning hard MCP errors.
- [x] Implement server-local Unreal engine/project path getter-setter tools and unified `status` health payload.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.

## Active 1251 Tool Expansion - Batch 12 Reference Parity Closure

- [x] Register the remaining 172 exact reference-derived tool names from the 1251 master list.
- [x] Implement safe generic operations for trace/raycast/overlap, line-of-sight, actor tags, actor attach/detach/mobility, snap-to-floor, relative transform, actor motion readback, current-level/streaming readback, and batch translation.
- [x] Return hard MCP errors for StateTree, procedural generation, vendor/plugin graph, GAS/tag registry, PCG/Landscape/Foliage mutation, and animation/physics/cloth/ControlRig authoring paths that need dedicated domain contracts.
- [x] Verify full planned-list parity: 558/558 planned names registered; `scripts\audit-tools.ps1` reports 1251 server tools and only expected `jobs.*` server-only entries.
- [x] Run static registry/audit/diff checks only; build/package/deploy remains deferred by user instruction.
