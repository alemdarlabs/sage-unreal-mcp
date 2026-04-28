# Sage Durum — Phase 1 + Phase 2 Tamam

> Scratch — diyagram her güncellemede overwrite. VS Code preview'da render olur (Markdown Preview Mermaid Support).

```mermaid
flowchart TB
  classDef done    fill:#16a34a,stroke:#15803d,color:#fff
  classDef now     fill:#f59e0b,stroke:#d97706,color:#fff
  classDef todo    fill:#cbd5e1,stroke:#64748b,color:#0f172a
  classDef hold    fill:#fde68a,stroke:#d97706,color:#0f172a,stroke-dasharray: 6 4
  classDef moat    fill:#a855f7,stroke:#7c3aed,color:#fff

  %% Phase 1 — execution layer
  subgraph P1["Phase 1 — Execution Layer ✓ (27 commit · 44 tool)"]
    direction TB
    M11["1.1 Server scaffolding<br/>HTTP+SSE · MCP · 4 base tool"]:::done
    M12["1.2 Plugin scaffolding<br/>UPlugin · WS · handshake · heartbeat"]:::done
    M13["1.3 Bridge + 30 domain tools<br/>actor · component · asset · editor · level · PIE · material"]:::done
    M14["1.4 Transaction layer<br/>begin/commit/rollback · bulk_modify · compare_and_set"]:::done
    M15a["1.5a Multi-editor MCP<br/>list/get/set_active_editor"]:::done
    M16a["1.6a Compile coordination<br/>Live Coding wrapper (Win) · Mac stub"]:::done
    M17["1.7 QA + Source Control<br/>list_tests · run_tests · checkout_files"]:::done
  end
  M11 --> M12 --> M13 --> M14 --> M15a --> M16a --> M17

  %% Phase 2 — knowledge layer (moat)
  subgraph P2["Phase 2 — Knowledge Layer (moat) ✓ (9 commit · 6 tool · 8K/16K graph)"]
    direction TB
    M21a["2.1a KuzuDB baseline ✓<br/>v0.11.3 prebuilt · libc++ workaround"]:::done
    M21b["2.1b GraphStore abstraction ✓<br/>variant&lt;Json,GraphError&gt; · envelope"]:::done
    M21c["2.1c Slot-scoped + schema migration ✓<br/>per-slot Kuzu · _SchemaVersion · v1→v3"]:::done
    M22["2.2 T1 indexing ✓<br/>AssetRegistry full scan → Asset · 8359 row · 24ms"]:::done
    M23a["2.3a DEPENDS_ON edges ✓<br/>16093 edge · UNWIND batched"]:::done
    M23b["2.3b Real-time delta ✓<br/>OnAssetAdded/Removed/Renamed → bridge event → graph patch"]:::done
    M24["2.4 Query tools ✓<br/>impact_of · references_to · find_unused"]:::moat
    M25["2.5 Cypher subset ✓<br/>query_graph · whitelist · *1..10 · 8KB cap"]:::done
  end
  M21a --> M21b --> M21c --> M22 --> M23a --> M23b --> M24 --> M25

  %% Geçiş
  M17 ==Demo 1 ✓==> M21a

  %% Şu an
  M25 ==> NOW(["ŞİMDİ BURADA<br/>━━━━━━━━━━━━━━━<br/>55 commit · 133 MCP tool<br/>Phase 1 + 2 + 3 + 4.0/4.1/4.2-r1+r2a..r2g(p1+p2+p3)/4.3/4.5-r1/4.6-r1+r2 CANLI<br/>SageTest UE 5.7 — 8K asset · 16K dep · 8337 class · 8336 inherit<br/>BP authoring + dispatcher + T3D + SCS deep CRUD · Material · Asset · Editor · Dialog<br/>UE-MCP 448 action audit → Phase 4 yol haritası %60 done"]):::now

  %% Phase 1'den ertelenenler
  subgraph DEF["Storage / out-of-process gerektirenler"]
    direction TB
    M15b["1.5b Slot management<br/>merge/migrate/prune<br/><i>SQLite slot store</i>"]:::hold
    M16b["1.6b Full restart orchestration<br/>save→shutdown→UBT→relaunch<br/><i>out-of-process compile</i>"]:::hold
  end
  M15a -.persistent.-> M15b
  M16a -.LC alternatif.-> M16b

  %% Phase 3 — execution polish + reflection
  subgraph P3["Phase 3 — Execution polish + T3 reflection (kısmen done)"]
    direction TB
    P3R["1.6b restart_editor orchestrator ✓<br/>save→build→kill→swap→relaunch · 25s · cross-platform"]:::done
    P3CH["class_hierarchy ✓<br/>schema v4 · 8337 UClass · 8336 INHERITS_FROM<br/>Pawn → Actor → Object verified"]:::done
    P3W["Windows cross-platform test"]:::todo
    P3PERF["Index perf — Kuzu COPY FROM JSON<br/>(30s → &lt;5s · Phase 4.4)"]:::todo
    P3SLOT["1.5b SQLite slot store<br/>merge/migrate/prune"]:::todo
  end
  NOW --> P3R
  NOW --> P3CH

  %% Phase 4 — full UE-MCP capability parity (no scope cuts)
  subgraph P4["Phase 4 — Full UE-MCP capability parity"]
    direction TB
    P40["4.0 UProperty collections ✓<br/>TArray/TMap/TSet · TObjectPtr/SoftObject · TSubclassOf<br/>FStruct shorthand · UEnum · raw-pointer dispatch"]:::done
    P41["4.1 Reflection ✓ (8 tool)<br/>reflect_class · reflect_struct · reflect_enum<br/>list_classes/structs/enums · find_implementers · CDO read"]:::done
    P42["4.2 Blueprint authoring (round 1+2a..2g/p1) ✓ (37 tool)<br/>r1: read/list_vars/fns/function graph/components/search<br/>add/delete var·fn · delete_node · connect_pins<br/>set_cdo_property · reparent · compile<br/>r2a: add_node · set/read_node_property · list_node_types<br/>r2b: list/add/delete_local_variable<br/>r2c: list/add/remove_interface<br/>r2d: list_graphs · rename_function<br/>r2e: list/add/remove_function_parameter (in+out, FnResult auto)<br/>r2f: create · create_interface (authoring loop closed)<br/>r2g/p1: list/add/remove_event_dispatcher (payload→r2h)<br/><i>round 2g remaining: ~11 tool (T3D, validate, SCS, set_var_props, ...)</i>"]:::done
    P43["4.3 Material round 1 ✓ (13 tool)<br/>read · params · expressions · create_instance<br/>add/delete/connect_expressions · set_base_color<br/>set_shading_model · validate · connect_texture<br/><i>round 2: build_graph · render_preview · shader_stats</i>"]:::done
    P45["4.5 Asset advanced round 1 ✓ (7 tool)<br/>mesh_bounds · mesh_collision · diagnose_registry<br/>list_redirectors · bulk_rename · move_folder · fixup_redirectors<br/><i>round 2: import_fbx/texture · datatable · FTS · sockets</i>"]:::done
    P46["4.6 Editor automation round 1+2 ✓ (11 tool)<br/>r1: console_command · take_screenshot · get_engine_version<br/>get_project_version · get_log_file_path · read_log<br/>r2 (dialog policy): set/clear/get_dialog_policy<br/>list_dialogs · respond_to_dialog<br/><i>round 3: sequencer · viewport · run_python · build_*</i>"]:::done
    P47["4.7 Project / engine introspection<br/>read_cpp_header · read_module · search_cpp<br/>engine source · INI tree · plugin enable/disable"]:::todo
    P48["4.8 Animation<br/>AnimBP · montage · sequence · blendspace<br/>IK Rig · ControlRig · skeleton · modifiers (~56 tool)"]:::todo
    P49["4.9 Niagara VFX<br/>system · emitter · modules · HLSL · renderer (~37 tool)"]:::todo
    P410["4.10 AI / Gameplay<br/>physics · nav · Enhanced Input · BT · EQS<br/>StateTree · SmartObject · perception · framework (~59 tool)"]:::todo
    P411["4.11 UMG / Widget · 4.12 PCG · 4.13 Landscape<br/>4.14 Foliage · 4.15 GAS · 4.16 Networking<br/>4.17 Audio · 4.18 Source control extras"]:::todo
    P419["4.19 Reporting / observability<br/>report_issue · session log · metrics"]:::todo
    P420["4.20 Headless test mode<br/>filesystem-only handlers + mock plugin coverage"]:::todo
    P44["4.4 Index perf — Kuzu COPY FROM JSON<br/>(transversal · 30s → &lt;5s)"]:::todo
  end
  P3CH --> P40
  P40 --> P41 & P42 & P43 & P45 & P46 & P47
  P40 --> P48 & P49 & P410 & P411
  P40 --> P419 & P420
  P40 -.transversal.-> P44

  %% Demolar
  M17  -. "Demo 1 — execution"  .- DEMO1[/"spawn · CAS · transactions · bulk · PIE guard<br/>SageTest'te ispatlandı"/]:::done
  M25  -. "Demo 2 — moat"       .- DEMO2[/"references_to(DefaultMaterial) → 5 mesh<br/>impact_of(depth=2) transitif<br/>top-5 referenced: VerseClass 300 refs<br/>banned keyword + unbounded * reddedildi"/]:::done

  %% Bu turun çıktıları
  subgraph TURN["Bu oturumun katkıları"]
    direction LR
    T1["Phase 2 baştan sona<br/>(2.1a → 2.5)"]:::done
    T2["6 yeni MCP tool<br/>(index_*, impact_of, references_to,<br/>find_unused, query_graph)"]:::done
    T3["5 integration smoke<br/>(graph · manager · indexer · cypher · kuzu)"]:::done
    T4["35/35 ctest yeşil"]:::done
  end
  NOW --- TURN
```

## MCP tool yüzeyi (52 tool toplam)

```mermaid
pie title Tool Domain Dağılımı (P1: 44 + P2: 6 + P3: 2)
  "Actor mutation" : 5
  "Component mutation" : 5
  "Asset mutation+lifecycle" : 8
  "Editor state+selection" : 6
  "Level" : 2
  "PIE" : 2
  "Material" : 1
  "Transactions+bulk+CAS" : 6
  "Multi-editor (local)" : 3
  "Compile (LC)" : 2
  "QA" : 2
  "Source control" : 2
  "Knowledge index (P2)" : 2
  "Knowledge query (P2)" : 4
  "Restart orchestrator (P3)" : 1
  "class_hierarchy (P3)" : 1
```

## Phase 4 hedef sürdürülen 51 tool dağılımı (UE-MCP audit'inden)

```mermaid
pie title Phase 4 Plan — yeni 51 tool
  "Tier A reflection + asset" : 15
  "Tier B Blueprint read" : 10
  "Tier C Material graph" : 6
  "Phase 4.0 UProperty collections" : 4
  "Phase 4.4 Kuzu COPY perf" : 1
  "Phase 4.5 Headless test mode" : 1
  "Tier D (Phase 5+)" : 14
```

## Knowledge graph şeması (Phase 2 v3)

```mermaid
erDiagram
  Asset {
    string path PK "SoftObjectPath: /Game/Foo.Foo"
    string kind     "Blueprint, StaticMesh, ..."
  }
  Class {
    string name PK
    string module
  }
  Module {
    string name PK
    string plugin
  }
  Plugin {
    string name PK
  }
  _SchemaVersion {
    int64 id PK
    int64 value
  }
  _IndexState {
    int64 id PK
    int64 last_indexed_at_ms
    int64 asset_count
    int64 dep_count
  }

  Asset ||--o{ Asset : "DEPENDS_ON (MANY_MANY)"
```

> v3 baseline. v4'te `Class INHERITS_FROM Class` + `Asset OF_CLASS Class` rel'leri (Phase 3 class_hierarchy).

## Renk kodu

- 🟢 **Yeşil** — bitti, gerçek UE 5.7 SageTest projesinde doğrulandı
- 🟠 **Turuncu** — şu anki çekilme noktası
- ⚫ **Gri** — Phase 3 todo
- 🟡 **Sarı kesikli** — storage / out-of-process bekliyor (1.5b SQLite, 1.6b restart)
- 🟣 **Mor** — Sage moat (intelligence layer farkı)

## Phase 2 commit zinciri

| Commit  | Milestone                                              |
|---------|--------------------------------------------------------|
| 3fd226b | 2.1a KuzuDB integration baseline                       |
| a0e81fb | 2.1b GraphStore abstraction                            |
| dbd392e | 2.1c Slot-scoped manager + schema migration            |
| cd9a56a | 2.2 T1 asset indexing                                  |
| 47c2899 | 2.3a DEPENDS_ON edges                                  |
| 176587d | 2.4 Query tools (impact_of/references_to/find_unused)  |
| c0ce18f | 2.3b Real-time AssetRegistry delta                     |
| 8f0d467 | 2.5 Cypher subset (query_graph)                        |
| c5a68d7 | fix(test): pin manager smoke to kCurrentSchemaVersion  |
