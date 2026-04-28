# Sage Phase 1 Durum — Şu Anda Neredeyiz

> Scratch — diyagram her güncellemede overwrite. VS Code preview'da render olur (Markdown Preview Mermaid Support).

```mermaid
flowchart TB
  classDef done    fill:#16a34a,stroke:#15803d,color:#fff
  classDef now     fill:#f59e0b,stroke:#d97706,color:#fff
  classDef todo    fill:#cbd5e1,stroke:#64748b,color:#0f172a
  classDef hold    fill:#fde68a,stroke:#d97706,color:#0f172a,stroke-dasharray: 6 4
  classDef moat    fill:#a855f7,stroke:#7c3aed,color:#fff

  %% Phase 1 spine
  subgraph P1["Phase 1 — Execution Full ✓"]
    direction TB
    M11["1.1 Server scaffolding<br/>HTTP+SSE · MCP · 4 base tool"]:::done
    M12["1.2 Plugin scaffolding<br/>UPlugin · WS client · handshake · heartbeat"]:::done
    M13a["1.3a Plugin↔Server bridge<br/>ixwebsocket · welcome + heartbeat_ack"]:::done
    M13b["1.3b Tool dispatch<br/>server route + UE plugin handler + e2e mock"]:::done
    M13c["1.3c Domain mutation tools<br/>5 actor · 5 component · 8 asset · 6 editor · 2 level · 2 PIE · 1 material"]:::done
    M14["1.4 Transaction layer<br/>begin/commit/rollback · bulk_modify · compare_and_set"]:::done
    M15a["1.5a Multi-editor MCP<br/>list/get/set_active_editor"]:::done
    M16a["1.6a Compile coordination<br/>Live Coding wrapper (Win) · Mac stub (-32007)"]:::done
    M17["1.7 QA + Source Control<br/>list_tests · run_tests · checkout_files"]:::done
  end

  M11 --> M12 --> M13a --> M13b --> M13c --> M14 --> M15a --> M16a --> M17

  %% Şu an
  M17 --> NOW(["ŞİMDİ BURADA<br/>24 commit · 44 tool<br/>Demo 1 SageTest&apos;te ÇALIŞIYOR<br/>actor_count fix verified"]):::now

  %% Phase 1 deferred
  subgraph DEF["Phase 2 storage gerektirenler"]
    direction TB
    M15b["1.5b Slot management<br/>merge/migrate/prune<br/><i>SQLite slot store gerek</i>"]:::hold
    M16b["1.6b Full restart orchestration<br/>save→shutdown→UBT→relaunch<br/><i>out-of-process compile</i>"]:::hold
  end

  M15a -.uçucu→persistent.-> M15b
  M16a -.LC alternatif.-> M16b

  %% Phase 2 moat
  subgraph P2["Phase 2 — Knowledge Layer (intelligence moat)"]
    direction TB
    M21a["2.1a KuzuDB baseline<br/>v0.11.3 prebuilt · CMake INTERFACE · smoke OK"]:::done
    M21b["2.1b GraphStore abstraction<br/>variant&lt;Json,GraphError&gt; · {rows,schema,row_count}"]:::done
    M21c["2.1c Slot-scoped DB + schema migration<br/>per-slot Kuzu path · _SchemaVersion node · v1 init"]:::now
    M22["2.2 T1 indexing (eager)<br/>AssetRegistry full scan → Asset/Class/Module/Plugin"]:::todo
    M23["2.3 T2 topology + real-time delta<br/>depends_on / inherits_from / implements + AssetRegistry events"]:::todo
    M24["2.4 High-level query tools<br/>impact_of · references_to · class_hierarchy · find_unused"]:::moat
    M25["2.5 Cypher subset (Layer 2)<br/>read-only AST whitelist · bounded *1..N · 8K cap"]:::todo
  end

  M21a --> M21b --> M21c --> M22 --> M23 --> M24 --> M25

  NOW ==Phase 2 başladı==> M21a

  %% Demo hedefleri
  M17 -. Demo 1 .- DEMO1[/"Demo 1 ispatlandı<br/>spawn · CAS · transactions · bulk · PIE guard"/]:::done
  M24 -. Demo 2 .- DEMO2[/"Demo 2 — moat<br/>impact_of(BP_Enemy) → safe delete"/]:::moat

  %% Bu turda eklenenler
  subgraph TOOLS["Bu turda eklenenler"]
    direction LR
    SK1["/unreal-close skill"]:::done
    SK2["/unreal-open skill"]:::done
    BUG["actor_count fix<br/>(sparse array)"]:::done
  end

  NOW --- TOOLS
```

## Phase 1 tool yüzeyi (44 tool)

```mermaid
pie title Tool Domain Dağılımı
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
```

## Renk kodu

- 🟢 **Yeşil** — bitti, gerçek UE 5.7 SageTest projesinde doğrulandı
- 🟠 **Turuncu** — şu anki çekilme noktası (hemen Phase 2'ye veya Windows test'e geçiş)
- ⚫ **Gri** — Phase 2 todo
- 🟡 **Sarı kesikli** — Phase 1 spec'inde olan ama Phase 2 storage layer'ına bağımlı (KuzuDB / SQLite gelmeden anlamsız)
- 🟣 **Mor** — Sage moat (intelligence layer farkı, execution'dan ayrışma)
