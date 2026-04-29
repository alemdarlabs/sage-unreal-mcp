# Sage Unreal MCP

## Uzman Agent Sistemi (ZORUNLU)

Bu projede `.claude/agents/` altında 13 uzman agent tanımlıdır. **Her konuşmada konuya göre ilgili agent otomatik çağrılmalıdır.** Kullanıcı bir soru sorduğunda veya bir konu hakkında konuştuğunda, o konunun uzmanı olan agent'ı `/agent-adi` skill olarak çağır ve o uzmanlıkla yanıt ver.

| Konu | Agent | Çağrı |
|---|---|---|
| Unreal C++ plugin, UCLASS/UPROPERTY/UFUNCTION reflection, AssetRegistry, FScopedTransaction, Slate, Live Coding | Unreal Architect | `/unreal-architect` |
| Modern C++23 server, CMake, vcpkg, async, sanitizers, performans | C++ Architect | `/cpp-architect` |
| MCP protocol, JSON-RPC envelope, transport (HTTP+SSE, WebSocket), tool schema design | MCP Protocol | `/mcp-protocol` |
| Knowledge graph şeması, KuzuDB, Cypher queries, indexing strategy | Graph Architect | `/graph-architect` |
| AI/LLM entegrasyonu, agent davranışı, MCP tool ergonomics | AI/ML Engineer | `/ai-engineer` |
| Şifreleme, güvenlik, auth, sandbox | Security Engineer | `/security-engineer` |
| WebSocket, real-time event streaming, heartbeat, reconnect | Real-time Engineer | `/realtime-engineer` |
| KuzuDB, SQLite, veri modeli, query optimizasyonu | Database Architect | `/db-architect` |
| Test stratejisi, QA, sanitizer kullanımı, edge case'ler | QA Engineer | `/qa-engineer` |
| CI/CD, build pipeline, vcpkg, plugin packaging, distribution | DevOps Engineer | `/devops-engineer` |
| Ürün önceliklendirme, MVP scope, feature kararları | Product Owner | `/product-owner` |
| Marka kimliği, görsel dil, logo, Sage brand family | Creative Director | `/creative-director` |
| Dokümantasyon, ADR, diagram, technical writing | Technical Writer | `/tech-writer` |

**Kurallar:**
- Kullanıcı açıkça bir konu hakkında konuşuyorsa, ilgili agent'ı Skill tool ile çağır
- Birden fazla alanı kapsayan sorularda en baskın alana ait agent'ı seç
- Agent çağrısı yapıldığında, o agent'ın uzmanlık perspektifinden yanıt ver
- Kullanıcı doğrudan `/agent-adi` yazarsa da ilgili agent çağrılır

## Overview

**Sage** is a family of MCP servers for game engines that adds an **intelligence layer** alongside execution. Most current AI dev tools let an agent run commands; Sage makes the agent *understand* the engine project (asset graph, references, impact analysis) and uses that understanding for precise, safe edits.

`sage-unreal-mcp` is the Unreal Engine implementation, the first of the family. Planned siblings: `sage-unity-mcp`, `sage-godot-mcp`.

**Status (2026-04-30): production code aktif** — Phase 1+2+3+4 + Milestone 1.5b tamamlandı, ilk gerçek dogfooding turu (11 gap fix tek oturumda) bitti, Mac→Windows production transition başlıyor. Mimari kararlar [`.claude/decisions/`](.claude/decisions/) (17 ADR), sistem dokümantasyonu [`.claude/docs/`](.claude/docs/), kabul edilen davranış kuralları [`.claude/notes/lessons.md`](.claude/notes/lessons.md).

## Ticari Bağlam (KRİTİK)

**Sage ticari satılacak ürün.** Bu, license + dağıtım + güvenlik kararlarını doğrudan etkiler:

- **Permissive open-source license (Apache-2.0, MIT, BSD) YASAK** — rakipler aynı kodu alıp ürünü yeniden satabilir, moat çürür. Erken ADR-016'da Apache-2.0 önerim **iptal edildi**.
- **Repo private** (`git@github.com:alemdarlabs/sage-unreal-mcp.git`); LICENSE dosyası yok, README'de proprietary copyright notice.
- **Dağıtım modeli iki katmanlı**:
  - *Binary*: npm public — `npm install -g @alemdarlabs/sage-mcp` herkese açık (Claude Code/Codex pattern). Postinstall GitHub Releases'tan native binary indirir.
  - *Runtime*: auth-gated — sage-server başladığında **login zorunlu**, sadece authenticate olmuş kullanıcılar tool çağırabilir. Auth detayı (OAuth / license key / hibrit) Phase 5+ Mahmut kararına bırakıldı; kod tarafında auth hooks **henüz yok**.
- **Distribution channel detayı**: ADR-016 (npm-first) kararı binary kanalı için geçerli; runtime auth ayrı katman olarak gelecek.
- **License/dağıtım önerisi yaparken** "ücretsiz public, rakip kullanım serbest" varsayımı YAPMA — daima commercial-protective lens.

## Tech Stack

| Katman | Teknoloji |
|---|---|
| Plugin | Unreal C++ (UPlugin) |
| Server | C++23 (modern, manuel MCP impl) |
| Knowledge graph | KuzuDB (embedded, Cypher) |
| Audit / index | SQLite |
| Server transport | HTTP + SSE (Streamable HTTP) |
| Plugin transport | WebSocket (FWebSocketsModule + uWebSockets) |
| Build system | CMake + vcpkg |
| Test | Catch2 + ASan/UBSan/TSan |
| JSON | nlohmann/json + simdjson |
| Logging | spdlog |

Detay: [`.claude/docs/tech-stack.md`](.claude/docs/tech-stack.md)

## Project Structure

```
sage-unreal-mcp/
├── CLAUDE.md                       # Bu dosya
├── README.md                       # Public-facing description
├── .claude/
│   ├── agents/                     # 13 uzman agent
│   ├── decisions/                  # ADR'ler
│   ├── docs/                       # Mimari dokümantasyon
│   ├── notes/                      # Scratch notlar, lesson, todo
│   └── skills/                     # Otomasyon skill'leri
├── plugin/                         # Unreal C++ plugin (TBD)
├── server/                         # C++23 MCP server (TBD)
├── tests/                          # Catch2 testler (TBD)
└── docs/                           # External / public docs (TBD)
```

Detay: [`.claude/docs/project-structure.md`](.claude/docs/project-structure.md)

## Şu Anki Durum (Snapshot)

> Tek satırda durum: **102 commit · 443 plugin tool · 457 server schema (444 _editor-aware) · 17 ADR · Phase 1+2+3+4 + Milestone 1.5b TAMAM · multi-editor per-call routing + bp.full_dump + bootstrap_module + UBT rebuild CANLI · ilk dogfooding turu (11 gap fix) bitti · Mac→Windows transition (2026-04-30)**.

- **Önceki test ortamı (Mac, dogfooding)**: `~/Developer/alemdarlabs/Kale/Kale.uproject` (Game Animation Sample, Motion Matching) + `~/Developer/alemdarlabs/SuperheroFlightAnimations/SuperheroFlightAnimations.uproject` (state machine + ActorComponent flight, dogfooding sırasında C++'a yükseltildi) + `~/Developer/alemdarlabs/SageTest/SageTest.uproject` (eski Third Person + Blueprint).
- **Şimdi (2026-04-30)**: Mahmut Windows tarafına geçti, **gerçek production project'lerde** Sage'i kullanmaya başlıyor. Bkz. auto-memory `feedback_real_projects_caution.md` (destructive op disiplini) ve `project_windows_transition.md` (platform farkları).
- **Knowledge graph (Mac SageTest slot'unda canlı)**: 8359 asset · 16093 DEPENDS_ON · 8337 UClass · 8336 INHERITS_FROM. Diğer slot'lar (Kale, SuperheroFlight, Windows projeleri) henüz indexlenmedi — `index_slot` çağrısı ile aktive olur.
- **Multi-editor (ADR-017)**: Tüm 444 editor-scoped tool opsiyonel `_editor` parametresi alıyor (session_id / label / instance_id / project-name prefix). Routing önceliği: explicit > active pointer > tek editor implicit > ambiguity error. 12 server-side tool (knowledge graph + ping + editor mgmt) `_editor` almaz.
- **İlk dogfooding turu sonuçları (2026-04-29, 11 gap, hepsi tek oturumda fix)**:
  - Pre: 235 invalid schema fix (nlohmann brace-init pitfall, `obj()` helper rewrite)
  - Gap #1+#2: `asset.search` query optional + `asset.list` class/kind/offset/fields filter+pagination+projection
  - Gap #3: multi-editor per-call routing (ADR-017) + silent `getClients()[0]` bug fix
  - Gap #4 + #5 + V5→V6: `project.create_cpp_class` `bootstrap_module` (BP-only → C++) + UHT prefix/header registry (~30 base class) + `BuildSettingsVersion.V6`
  - Gap #6+#7+#8+#11: `bp.full_dump` atomic snapshot + CDO fix + response collapse + T3D `include_all_nodes`
  - Gap #9: `project.add_module_dependency` Build.cs array-literal-aware insertion + `private` flag
  - Gap #10: `restart_editor` `rebuild_project_modules` (Mac UBT compile, Live Coding muadili)
- **Açık iş (Phase 5 / paketleme öncesi)**:
  - `asset.migrate` tool (`FAssetToolsModule::MigratePackages` wrapper)
  - MCP transport polish (`Mcp-Session-Id` header + GET /mcp SSE + OAuth metadata stub + `Mcp-Protocol-Version`)
  - License kararı (ADR-016 Apache-2.0 önerim — onay bekliyor)
  - Public docs/getting-started + README rakam sync (eski 82/443/454 → 102/443/457)
  - `project.get_info` disconnected mode
  - Repo public push (`git@github.com:alemdarlabs/sage-unreal-mcp.git` boş repo, henüz push edilmedi)
- **Skills**: `/unreal-close` + `/unreal-open` editor restart loop autonomous (Mac); MCP tool olarak `restart_editor` (Mac+Win, yeni `rebuild_project_modules` flag).
- **Dokümantasyon**:
  - [`.claude/notes/diagram.md`](.claude/notes/diagram.md) — milestone akışı + tool dağılımı + knowledge graph şeması
  - [`.claude/notes/ue-mcp-integration-plan.md`](.claude/notes/ue-mcp-integration-plan.md) — UE-MCP 562 action'a karşı Sage yol haritası
  - [`.claude/notes/ue-mcp-tasks.md`](.claude/notes/ue-mcp-tasks.md) — per-tool task listesi
  - [`.claude/notes/lessons.md`](.claude/notes/lessons.md) — kabul edilen kurallar (en kritik: MVP scope-cut yasak, BP/Material GameThread marshal, nlohmann brace-init pitfall, MCP Streamable HTTP partial-impl, BridgeServer routing TODO trap, DRY middleware injection, C++ proje plugin install Source+Binaries)
  - [`.claude/decisions/`](.claude/decisions/) — 17 ADR (son: ADR-017 multi-editor-routing-impl, ADR-016 distribution-channel-npm)
- **Yeni session devraldığında ilk bakılacak**: bu dosya → auto-memory (`sage_current_state.md` + `project_windows_transition.md` + `feedback_real_projects_caution.md`) → `.claude/notes/diagram.md` → `.claude/decisions/adr-017-multi-editor-routing-impl.md` → son commit `git log --oneline -12`.

## Tool Tablosu (özet — 457 toplam, 444 _editor-aware + 13 server-only)

| Phase | Domain | Tool sayısı |
|---|---|---|
| 1 | Actor/Component/Asset/Editor/Level/PIE/Material/Tx/CAS/Multi-editor/Compile/QA/SCM | 44 |
| 2 | Knowledge graph (index_*, impact_of, references_to, find_unused, query_graph) | 6 |
| 3 | restart_editor + class_hierarchy | 2 |
| 4.1 | Reflection (reflect_class/struct/enum, list_*, find_implementers, CDO) | 8 |
| 4.2-r1 | Blueprint authoring round 1 (`bp.*`) | 17 |
| 4.3 | Material graph authoring (`mat.*`) | 13 |
| 4.5-r1 | Asset advanced round 1 (`asset.*`) | 7 |
| 4.6-r1 | Editor automation round 1 (`editor.*`) | 6 |
| 4.6-r2 | Dialog policy (`editor.set_dialog_policy` / clear / get / list_dialogs / respond_to_dialog) | 5 |
| 4.2-r2a | BP graph node CRUD (`bp.add_node` / set_node_property / read_node_property / list_node_types) | 4 |
| 4.2-r2b | BP local variables (`bp.list_local_variables` / add_local_variable / delete_local_variable) | 3 |
| 4.2-r2c | BP interface CRUD (`bp.list_interfaces` / add_interface / remove_interface) | 3 |
| 4.2-r2d | BP graph management (`bp.list_graphs` / rename_function) | 2 |
| 4.2-r2e | BP function parameter I/O (`bp.list_function_parameters` / add_function_parameter / remove_function_parameter) | 3 |
| 4.2-r2f | BP asset creation (`bp.create` / `bp.create_interface`) | 2 |
| 4.2-r2g/p1 | BP event dispatcher CRUD (`bp.list_event_dispatchers` / add_event_dispatcher / remove_event_dispatcher) | 3 |
| 4.2-r2g/p2 | BP T3D node clipboard (`bp.export_nodes_t3d` / `bp.import_nodes_t3d`) | 2 |
| 4.2-r2g/p3 | BP SCS-component deep CRUD (`bp.read_component_properties` / `get_component_property` / `reparent_component`) | 3 |
| 4.2-r2g/p4 | BP diagnostics + dry-run (`bp.validate` / `bp.run_construction_script`) | 2 |
| 4.2-r2g/p5 | BP var-flag + CDO + deps (`bp.set_variable_properties` / `bp.get_cdo_properties` / `bp.get_dependencies`) + UE 5.0 PC_Real subcategory fix | 3 |
| 4.7-p1 | Project introspection (`project.get_info` / `list_modules` / `read_cpp_header` / `read_cpp_source`) | 4 |
| 4.7-p2 | Engine source + search (`project.search_cpp` / `list_engine_modules` / `read_engine_header` / `find_engine_symbol`) | 4 |
| 4.7-p3 | INI config tree (`project.read_config` / `search_config` / `list_config_tags`) | 3 |
| 4.7-p4 | INI write + plugin enable (`project.set_config` / `set_plugin_enabled`) | 2 |
| 4.6-r3-b1 | Editor state control (`editor.undo` / `redo` / `focus_on_actor` / `set_viewport`) | 4 |
| 4.6-r3-b2 | Runtime state mutation (`editor.set_property` / `set_pie_time_scale`) | 2 |
| 4.6-r3-b3 | Log + crash forensics (`editor.search_log` / `list_crashes` / `check_for_crashes` / `get_crash_info`) | 4 |
| 4.6-r3-b4 | Level building (`editor.build_all` / `build_geometry` / `build_lighting` / `build_hlod` / `get_build_status`) | 5 |
| 4.6-r3-b5 | Python scripting (`editor.run_python`) | 1 |
| 4.6-r3-b6 | Sequencer minimal (`seq.create` / `seq.list_tracks` / `seq.add_track`) | 3 |
| 4.5-r2-b1 | Asset query (`asset.list` / `asset.search` / `asset.read_properties`) | 3 |
| 4.5-r2-b2 | Mesh sockets (`asset.list_sockets` / `add_socket` / `remove_socket` — Static + Skeletal) | 3 |
| 4.5-r2-b3 | Textures (`asset.list_textures` / `get_texture_info` / `set_texture_settings`) | 3 |
| 4.5-r2-b4 | Asset writes (`asset.create_data_asset` / `delete_batch` / `reload_package`) | 3 |
| 4.5-r2-b5 | Mesh material slots (`asset.list_mesh_materials` / `set_mesh_material` / `set_sk_material_slots`) | 3 |
| 4.5-r2-b6 | DataTable (`asset.read_datatable` / `create_datatable` / `reimport_datatable`) | 3 |
| 4.5-r2-b7 | Import + reimport (`asset.import_texture` / `asset.reimport`) | 2 |
| 4.5-r2-b8 | Export (`asset.export` — auto FBX/PNG/TGA/EXR/WAV) | 1 |
| 4.5-r2-b9 | FBX import (`asset.import_static_mesh` / `import_skeletal_mesh` / `import_animation`) | 3 |
| 4.11-r1 | UMG starter (`widget.create` / `widget.list` / `widget.read`) | 3 |
| 4.11-r2 | UMG authoring (`widget.add_widget` / `widget.remove_widget` / `widget.set_property`) | 3 |

## Build & Run Commands

### Mac (test ortamı, dogfooding)

```bash
# Server build
VCPKG_ROOT=$HOME/vcpkg cmake --build --preset debug --target sage-server

# Server start
SAGE_REPO_ROOT=/Users/mahmutalemdar/Developer/alemdarlabs/sage-unreal-mcp \
SAGE_UE_ROOT="/Users/Shared/Epic Games/UE_5.7" \
SAGE_LOG_LEVEL=info \
./build/debug/bin/sage-server >/tmp/sage-server.log 2>&1 &

# Plugin build (UAT BuildPlugin, ~70s)
./scripts/build-plugin.sh

# Plugin install (her UE projesi için 3 şey kopyala — lessons.md'deki kural)
PROJ=/path/to/Project
mkdir -p "$PROJ/Plugins/SageBridge/Binaries/Mac"
cp build/plugin/SageBridge.uplugin "$PROJ/Plugins/SageBridge/"
cp build/plugin/Binaries/Mac/* "$PROJ/Plugins/SageBridge/Binaries/Mac/"
rm -rf "$PROJ/Plugins/SageBridge/Source"
cp -R build/plugin/Source "$PROJ/Plugins/SageBridge/Source"  # C++ projelerde şart

# Editor restart loop (otonom MCP tool)
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"restart_editor","arguments":{"confirmed":true}}}' \
  http://127.0.0.1:7777/mcp
```

### Windows (production hedefi — 2026-04-30 itibarıyla)

```powershell
# Server build
$env:VCPKG_ROOT = "$HOME\vcpkg"
cmake --build --preset debug --target sage-server

# Server start
$env:SAGE_REPO_ROOT = "C:\path\to\sage-unreal-mcp"
$env:SAGE_UE_ROOT   = "C:\Program Files\Epic Games\UE_5.7"
$env:SAGE_LOG_LEVEL = "info"
.\build\debug\bin\sage-server.exe

# Plugin build
.\scripts\build-plugin.ps1

# Plugin install (3 şey, Win64 platform)
$proj = "C:\path\to\Project"
New-Item -Type Directory "$proj\Plugins\SageBridge\Binaries\Win64" -Force
Copy-Item build\plugin\SageBridge.uplugin "$proj\Plugins\SageBridge\"
Copy-Item build\plugin\Binaries\Win64\* "$proj\Plugins\SageBridge\Binaries\Win64\"
Remove-Item -Recurse "$proj\Plugins\SageBridge\Source" -ErrorAction SilentlyContinue
Copy-Item -Recurse build\plugin\Source "$proj\Plugins\SageBridge\Source"  # C++ projelerde şart
```

### Mac vs Windows — Önemli Farklar

| Konu | Mac | Windows |
|---|---|---|
| **Live Coding** | YOK (UE 5.7 macOS desteklemiyor) → `restart_editor({rebuild_project_modules:true})` ile UBT compile | VAR — `compile_and_reload` çalışır, hot reload mümkün |
| **Plugin binary** | `UnrealEditor-SageBridge.dylib` | `UnrealEditor-SageBridge.dll` |
| **UBT script** | `<UE>/Engine/Build/BatchFiles/Mac/Build.sh` | `<UE>/Engine/Build/BatchFiles/Build.bat` |
| **kPlatformDir** | `Mac` | `Win64` |
| **UE install path** | `/Users/Shared/Epic Games/UE_5.7/` | `C:\Program Files\Epic Games\UE_5.7\` |
| **Modül lock** | `.dylib` memory-mapped → editor close şart swap için | `.dll` Live Coding ile hot-swap mümkün |

Sage codebase Windows-aware (`restart_orchestrator.cpp` per-platform `kPluginDylib`/`kUbtBuildScript`, plugin `.uplugin` `PlatformAllowList: [Win64, Mac, Linux]`). Ama tüm flow'un Windows'ta sıfırdan denemesi henüz yapılmadı — ilk Windows session'da UBT path normalization + plugin packaging UAT'ın Windows davranışı doğrulanmalı.

## Production Project Disiplini (KRİTİK)

Mac'te dogfooding sırasında SageTest + SuperheroFlightAnimations gibi **throwaway sample project'ler** kullandık; "Source/'u sileyim, fresh state'e dönüyorum" gibi destructive eylemler serbestti. **Windows'a geçtikten sonra Mahmut gerçek production project'lerde Sage kullanıyor** (yıllarca emek, takım/source-control, geri dönüşü olmayan kayıp riski). Sıkı disiplin:

1. **Destructive op öncesi `bp.full_dump` ZORUNLU** — Blueprint'i C++'a çevirmeden, silmeden, reparent etmeden önce mutlaka `output_path: "Saved/SageDumps/<name>.PRE_<op>.dump.json"` ile dump al; `include_t3d=true` paste-back için. Bu safety net olmadan continue etme.
2. **`confirmed:true` flag'lerini agent otomatik onaylamaz** — `restart_editor`, `delete_actor`, `delete_asset`, `asset.delete_batch`, `discard_changes` gibi tool'lar her seferinde explicit kullanıcı onayı ister.
3. **Manuel filesystem manipulation (`rm -rf`, uproject Modules silme, Source/ wipe) production'da yasak** — önce kullanıcıya sor.
4. **Multi-editor isim çakışması** — production project + Sage geliştirme repo'su aynı anda açıkken `_editor` parametresi her tool çağrısında explicit. Implicit fallback (tek editor) production senaryosunda riskli.
5. **`SAGE_UE_ROOT` + `SAGE_REPO_ROOT` env var ile server başlat** — paths tutmak için; production project SAGE_REPO_ROOT'a karışmasın.
6. **Major mutation öncesi source control kontrol** (`git status` veya equivalent) — clean working tree yoksa kullanıcıdan onay iste.
7. **Knowledge layer'dan yararlan** — körü körüne mutate etmek yerine `index_slot` + `references_to(...)` ile etki çıkar. `bp.full_dump` ile kombinlenince double safety net.

## İlk Dogfooding Turu Çıktıları (2026-04-29)

Mac'te ikinci Claude Code session'ı (Kale projesi) gerçek MCP-client testi yaptı; 11 gerçek gap raporu, hepsi tek oturumda fix edildi:

| Gap | Konu | Commit |
|---|---|---|
| pre | 234 invalid schema (nlohmann brace-init pitfall, `obj()` helper rewrite) | `a83fa00` |
| #1+#2 | `asset.search` query optional + `asset.list` class/kind/offset/fields | `c837969` |
| #3 | Multi-editor per-call routing (ADR-017) + silent `getClients()[0]` bug | `76ef244` + `930e46e` |
| #4 | `project.create_cpp_class` `bootstrap_module` (BP-only → C++) | `3f87ce5` |
| #5 | UHT prefix + parent header registry (~30 base class) | `6082c5e` |
| (V5→V6) | `BuildSettingsVersion.V6` (UE 5.7 default) | `5def2b5` |
| #6 | `bp.full_dump` atomic Blueprint snapshot (10+ helper orchestrate) | `c79bbe8` |
| #7 | `bp.full_dump` CDO + cosmetic cleanup | `aadf0bc` |
| #8 | `bp.full_dump` response collapse when output_path set | `c0b5269` |
| #9 | `project.add_module_dependency` Build.cs array-literal-aware + private flag | `409a48b` |
| #10 | `restart_editor` `rebuild_project_modules` (Mac UBT compile, Live Coding muadili) | `d3859df` |
| #11 | `bp.full_dump` `include_t3d` gerçekten T3D üretiyor (`include_all_nodes`) | `690318a` |

**Pattern (yeni dogfooding'lerde tekrar)**: Test eden Claude `Gap #N` formatıyla rapor — *Hedef* + *Denenen tool(lar)* + *Args* + *Sonuç/hata* + *Eksik* + *Öneri (A/B/C öncelikli)* + *Workaround*. Geliştirici Claude fix + commit + bildirim. Bu format çok değerliydi, korunmalı.

## Environment Variables

```bash
# SAGE_DATA_DIR=~/.sage-mcp                # KuzuDB + SQLite storage path
# SAGE_LOG_LEVEL=info                      # spdlog level
# SAGE_HTTP_PORT=7777                      # MCP HTTP+SSE port
# SAGE_WS_PORT=7778                        # WebSocket port for plugins
```

## Code Conventions

- **C++23**: concepts, `std::expected`, ranges, modules (when supported)
- **Style**: clang-format (`.clang-format` in repo root, TBD)
- **Lint**: clang-tidy (`.clang-tidy`, TBD)
- **Naming**: PascalCase classes, camelCase methods, UPPER_SNAKE constants
- **Headers**: `#pragma once`, no include guards
- **Errors**: `std::expected<T, E>` for fallible operations; exceptions only for unrecoverable
- **No raw new/delete**: smart pointers, RAII
- **Const-correctness**: aggressive

## Working Rules

### 1. Plan Mode Default
- Enter plan mode for any non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STOP and re-plan immediately — don't keep pushing
- Use plan mode for verification steps, not just building
- Write detailed specs upfront to reduce ambiguity

### 2. Subagent Strategy
- Use subagents liberally to keep main context window clean
- Offload research, exploration, parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One task per subagent for focused execution

### 3. Self-Improvement Loop
- After ANY correction from the user: update `.claude/notes/lessons.md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start

### 4. Verification Before Done
- Never mark a task complete without proving it works
- Run tests, check sanitizer output, demonstrate correctness
- Ask: "Would a senior engineer approve this?"

### 5. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: redo it
- Skip this for simple, obvious fixes — don't over-engineer
- Challenge your own work before presenting it

### 6. Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests — then resolve them
- Zero context switching required from the user

### 7. Türkçe Yanıt Zorunlu
- Mahmut türkçe konuşur; yanıtlar türkçe olmalı (orthografik tam — diakritik şart, "fur" yerine "için", "loeschen" yerine "sil" yazma)
- Teknik terimler ve kod identifier'ları orijinal hâlinde kalır
- Commit mesajları teknik diline uygun (genelde İngilizce + Türkçe açıklama mix), README/docs İngilizce — bunlar dış-yüzlü

### 8. MVP Scope-Cut Yasak
- "1-week MVP'ye sığsın" diye feature'ı read+write parçalara bölme — read+write hep birlikte ship
- "v1 minimum", "first cut" framing'i kullanma; bir feature dark corner'larıyla beraber teslim
- Time estimate ≠ scope deletion gerekçesi
- Mahmut'un sözü: *"bir daha bir MVP'ye sığdırmak için bir şey yapma, sana ne amk? Sen işini yap!"*

### 9. Production Project Destructive Guard
- Yukarıdaki "Production Project Disiplini" bölümü içeriği (özet): destructive op öncesi `bp.full_dump` zorunlu, `confirmed:true` flag'leri otomatik onaylanmaz, manuel `rm -rf` veya uproject Modules silme yasak (önce kullanıcıya sor), multi-editor ortamında `_editor` parametresi explicit
- Mac sample'larda yaptığımız "deneyip görelim" davranışı production'da geçersiz

## Task Management

1. **Plan First**: Write plan to `.claude/notes/todo.md` with checkable items
2. **Verify Plan**: Check in before starting implementation
3. **Track Progress**: Mark items complete as you go
4. **Explain Changes**: High-level summary at each step
5. **Document Results**: Add review section to `.claude/notes/todo.md`
6. **Capture Lessons**: Update `.claude/notes/lessons.md` after corrections

## Core Principles

- **Simplicity First**: Make every change as simple as possible. Impact minimal code.
- **No Laziness**: Find root causes. No temporary fixes. Senior developer standards.
- **Intelligence as Moat, Execution as Foundation**: Sage'in diferansiyatörü intelligence layer (knowledge graph). Ama implementation sıralaması execution-first (ADR-012) — önce agent eyleyebilsin, sonra anlayışı üstüne dökülür. Phase 1 = execution, Phase 2 = knowledge.
- **Token Discipline**: Every tool response respects the optimization principles in [`.claude/docs/api-spec.md`](.claude/docs/api-spec.md). No bloat.

## Documentation

- [Architecture](.claude/docs/architecture.md) — Sistem mimarisi (topology, lifecycle, multi-editor)
- [Tech Stack](.claude/docs/tech-stack.md) — Teknoloji kararları ve gerekçeleri
- [API Spec](.claude/docs/api-spec.md) — MCP tool catalog, transport protocols, token optimization
- [Database Schema](.claude/docs/database-schema.md) — KuzuDB graph schema + SQLite tables
- [Project Structure](.claude/docs/project-structure.md) — Klasör yapısı ve konvansiyonlar
- [Knowledge Graph](.claude/docs/knowledge-graph.md) — 3-tier indexing detayı + schema
- [Transactions](.claude/docs/transactions.md) — Transaction layer detayı
- [Compile Coordination](.claude/docs/compile-coordination.md) — Live Coding vs full restart
- [MVP Roadmap](.claude/docs/mvp-roadmap.md) — Phase 1 + Phase 2 milestone breakdown, risk register
- [Decisions (ADR)](.claude/decisions/) — Architectural Decision Records (17 ADRs · son: ADR-017 multi-editor-routing-impl)

## Kritik Dersler Özeti (lessons.md'den)

Tam liste [`.claude/notes/lessons.md`](.claude/notes/lessons.md) — schema/transport/build/UE-API touch eden değişiklikten önce zorunlu okuma. En kritik 8 madde:

1. **BP/Material mutation GameThread'e marshal** — UE editor mutation API'leri (FBlueprintEditorUtils, UObject Modify, FScopedTransaction, SCS edits) WS worker thread'inden çağrılırsa editor anında crash eder. Plugin handler'ları `detail::RunOnGameThread([]() { ... })` ile sarmalı (zaten `GT(...)` register wrapper'ı bunu yapıyor).
2. **macOS .dylib swap → editor restart şart** — UE 5.7 macOS'ta plugin .dylib memory-mapped; fresh build/copy editor process exit etmedikçe etkili olmaz. `restart_editor` MCP tool veya `/unreal-close + /unreal-open` skill ikilisi otonom.
3. **C++ proje plugin install: 3 şey** — `*.uplugin` + `Binaries/<Platform>/*.dylib(.modules)` + **`Source/`** (C++ projelerde mecbur, BP-only'da opsiyonel). UBT proje target'ında plugin'i source'tan rebuild ediyor; sadece binary swap C++ proje açılışında "could not compile plugin" verir.
4. **nlohmann brace-init pitfall** — `{{"a","b"}}` two strings → `{"a":"b"}` object (NOT `["a","b"]` array). String-array required field'larda `std::initializer_list<const char*>` parametresi kullan, `obj({})` argümanı JSON null'a init eder (empty object DEĞİL). `tools/list` output Zod-validate edilmeli; smoke `tools/call` direct invoke schema'yı atlar.
5. **MCP Streamable HTTP partial-impl (paketleme blocker)** — sage-server şu an `Mcp-Session-Id` response header, GET `/mcp` SSE endpoint, OAuth metadata stub, `Mcp-Protocol-Version` header üretmiyor. Schema validation bittiğinde Claude Code stateless mode'da çalışıyor ama paketleme öncesi tamamlanmalı.
6. **PC_Real subcategory zorunlu** — UE 5.0+ pin type'lara `PC_Real + (PC_Float | PC_Double)` set etmek zorunlu. `PinCategory = FName(*UserStr)` direct assignment KismetCompiler crash + corrupt asset + crash loop. `MakePinType()` helper kullan.
7. **DRY middleware injection > per-tool repetition** — Cross-cutting param (`_editor`, auth header, vs.) registry/response layer'da bir kez uygulan. Per-tool source repetition lessons.md'deki "234 invalid schema" felaketinin kaynağı.
8. **Silent fail anti-pattern** — `bp.full_dump`'ın CDO + T3D çağrıları sessizce yutuluyordu (Gap #7, #11). Hata durumunda en az `_skip_reason` field'ı set et ki kullanıcı sebebi görsün. TODO'lar (`getClients()[0]`-tier) failing test veya error path'a wire'lı olmalı.

## Yeni Session Devraldığında

İlk 5 dakikada bakılacaklar (sıralı):

1. **Bu dosya** (CLAUDE.md) — full snapshot + ticari bağlam + production disiplini + Windows/Mac build + dogfooding turu çıktıları
2. **`.claude/notes/lessons.md`** — kabul edilmiş kurallar (yukarıdaki 8 madde + onlarca daha)
3. **`.claude/decisions/adr-017-multi-editor-routing-impl.md`** — son büyük mimari karar
4. **`.claude/decisions/adr-016-distribution-channel-npm.md`** — dağıtım stratejisi (binary kanalı)
5. **Son commit'ler**: `git log --oneline -12`
6. **Server canlı mı**: `curl -sS -X POST -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' http://127.0.0.1:7777/mcp | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['tools']))"` → 457 olmalı
7. **Bağlı editor'ler**: `list_editors` MCP tool çağrısı

Yeni gap raporu / feature isteği geldiğinde:
- Test eden Claude `Gap #N` formatında raporluyorsa → fix + commit + bildirim pattern'i
- Yeni feature → ADR taslağı önce, sonra implement
- Schema değişikliği → `tools/list` Zod-validate kontrol şart
- Plugin handler değişikliği → BuildPlugin + dylib+Source swap + editor restart loop
