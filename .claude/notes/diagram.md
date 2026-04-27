# Sage — Big Picture

> Bu dosya scratch — her yeni diyagram sürekli buraya yazılır (önceki overwrite olur).
> Kalıcı diyagramlar `.claude/docs/` veya `.claude/decisions/` içine embed edilir.
> VS Code'da `Cmd+Shift+V` ile preview aç, "Markdown Preview Mermaid Support" extension yüklü olsun.

---

## 1. System Topology — Bileşenler ve Bağlantılar

Kullanıcıdan Unreal Editor'e tüm yol. Sage server kalıcı, editor ephemeral, knowledge graph slot başına izole.

```mermaid
flowchart TD
    subgraph User["👤 Kullanıcı / Geliştirici"]
        Term["Terminal<br/>(Claude Code, Cursor)"]
    end

    subgraph Server["🟪 Sage Server (kalıcı C++23 prosesi)"]
        MCP[MCP Protocol Layer]
        Router[Tool Router]
        Tools[Tool Implementations]
        Lifecycle[Editor Lifecycle Manager]
        WSS[WebSocket Server]
        OpQ[(Operation Queue<br/>SQLite)]
        Audit[(Audit Log<br/>SQLite)]
        Slot[(Slot Index<br/>SQLite)]
        KG[(Knowledge Graph<br/>KuzuDB - Phase 2)]

        MCP --> Router
        Router --> Tools
        Tools --> OpQ
        Tools --> Audit
        Tools -.-> KG
        Router --> Lifecycle
        Lifecycle --> Slot
        Lifecycle --> WSS
    end

    subgraph EditorA["🟧 UE Editor — host"]
        PluginA["Sage Bridge Plugin (C++)"]
        ARLA[AssetRegistry Listener]
        TxA[UTransactor Wrapper]
        WSCA[WS Client]
        PluginA --> ARLA
        PluginA --> TxA
        PluginA --> WSCA
    end

    subgraph EditorB["🟧 UE Editor — client (opsiyonel)"]
        PluginB[Sage Bridge Plugin]
    end

    Term -->|"MCP / HTTP+SSE<br/>kalıcı bağlantı"| MCP
    WSS <-->|"WebSocket / JSON-RPC<br/>localhost"| WSCA
    WSS <-->|"WebSocket / JSON-RPC"| PluginB

    classDef store fill:#e1f5ff,stroke:#0288d1,color:#000
    classDef phase2 stroke-dasharray: 5 5
    class OpQ,Audit,Slot,KG store
    class KG phase2
```

> Kesik çizgili kutu (KuzuDB) = Phase 2'de devreye girer. Phase 1'de minimum slot identity tracking var, knowledge graph queries yok.

---

## 2. Tool Taxonomy — Hangi Yetenekler Hangi Katmanda

3 ana grup: Phase 1 execution, Phase 2 knowledge, Phase 0 (her zaman) infrastructure.

```mermaid
flowchart TD
    Sage((Sage MCP)) --> P1[Phase 1<br/>Execution Tools]
    Sage --> P2[Phase 2<br/>Knowledge Tools]
    Sage --> Inf[Infrastructure Tools]

    P1 --> Mut["Mutation<br/>actor / component / asset / material"]
    P1 --> Tx["Transactions<br/>begin / commit / rollback / bulk"]
    P1 --> Life["Lifecycle<br/>list_editors / slot mgmt / compile"]
    P1 --> St["Editor State<br/>world / viewport / PIE / tests"]
    P1 --> Save["Save & Source Control<br/>save_assets / auto-checkout"]

    P2 --> HL["High-level Queries<br/>impact_of / references_to /<br/>class_hierarchy / find_*"]
    P2 --> CS["Cypher Subset<br/>query() — read-only sandbox"]

    Inf --> Disc["Discovery<br/>list_editors / get_active / set_active"]
    Inf --> Idx["Indexing<br/>status / reindex / configure"]
    Inf --> Aud["Audit<br/>history / inspect / revert"]

    classDef phase1 fill:#fff3e0,stroke:#e65100,color:#000
    classDef phase2 fill:#e8f5e9,stroke:#2e7d32,color:#000
    classDef infra fill:#e3f2fd,stroke:#0277bd,color:#000
    classDef root fill:#f3e5f5,stroke:#6a1b9a,color:#000,font-weight:bold
    class Sage root
    class P1,Mut,Tx,Life,St,Save phase1
    class P2,HL,CS phase2
    class Inf,Disc,Idx,Aud infra
```

---

## 3. MVP Roadmap — Zaman Çizelgesi

ADR-012'ye göre execution-first. Phase 1 tamamlandığında Demo 1 ("AI ne dersem yapıyor"), Phase 2 sonunda Demo 2 ("AI projeyi anlıyor + yapıyor").

```mermaid
gantt
    title Sage MVP Roadmap
    dateFormat YYYY-MM-DD
    axisFormat %b %d

    section Phase 1 — Execution Full
    1.1 Server scaffolding         :p1a, 2026-04-28, 7d
    1.2 Plugin scaffolding         :p1b, after p1a, 7d
    1.3 Core mutation tools        :p1c, after p1b, 7d
    1.4 Transaction layer          :p1d, after p1c, 7d
    1.5 Multi-editor + lifecycle   :p1e, after p1d, 7d
    1.6 Compile coordination       :p1f, after p1e, 7d
    1.7 PIE + tests + SCM (buffer) :p1g, after p1f, 7d
    Demo 1 — Full execution        :milestone, m1, after p1g, 0d

    section Phase 2 — Knowledge Layer
    2.1 KuzuDB integration         :p2a, after m1, 7d
    2.2 T1 indexing                :p2b, after p2a, 7d
    2.3 T2 + AssetRegistry events  :p2c, after p2b, 7d
    2.4 High-level queries         :p2d, after p2c, 7d
    2.5 Cypher subset Layer 2      :p2e, after p2d, 7d
    Demo 2 — Moat proven           :milestone, m2, after p2e, 0d
```

---

## 4. Örnek Tool Call Akışı

Kullanıcı "BP_Enemy'nin Health'ini 200 yap" dediğinde sistemin uçtan uca akışı. Tek bir mutation tool çağrısı, transaction layer ve audit log dahil.

```mermaid
sequenceDiagram
    actor User as 👤 Kullanıcı
    participant Claude as Claude Code
    participant Server as Sage Server
    participant Audit as Audit Log
    participant Plugin as UE Plugin
    participant Editor as Unreal Editor

    User->>Claude: "BP_Enemy'nin Health'ini 200 yap"
    Claude->>Server: modify_actor_property(actor, "Health", 200)
    activate Server
    Server->>Server: validate args, route to slot
    Server->>Audit: log tx start (status: pending)
    Server->>Plugin: { tx_id, tool, args }
    activate Plugin
    Plugin->>Editor: FScopedTransaction open
    Plugin->>Editor: Actor->Modify() (snapshot)
    Plugin->>Editor: Actor->Health = 200
    Plugin->>Editor: scope close (commit)
    Plugin->>Server: { tx_id, status: "committed", hashes }
    deactivate Plugin
    Server->>Audit: log tx complete
    Server->>Claude: { tx_id, success, undo_handle }
    deactivate Server
    Claude->>User: "Done. Ctrl+Z ile geri alabilirsin."

    Note over User,Editor: Phase 2'de bu akışa<br/>knowledge graph update'i eklenir
```

---

## Özet: Sage Nedir?

**Sözel:** Unreal Engine için MCP server family'sinin ilk üyesi. AI agent'ların Unreal projesini anlayıp güvenli mutate etmesini sağlar.

**Mimari özet:**
- Persistent C++23 server + UE C++ plugin köprüsü
- HTTP+SSE (Claude'a) + WebSocket (plugin'e) transports
- KuzuDB (knowledge graph) + SQLite (audit, slot index)
- Multi-editor support, transaction-safe, undo-integrated

**Yol haritası:**
- Phase 1 (~7 hafta): Full execution — agent komut alıp eyleyebilir
- Phase 2 (~5 hafta): Knowledge layer — agent projeyi anlayabilir
- Toplam ~12 hafta MVP, sonra Sage Unity / Sage Godot family

**Moat:** Çoğu engine MCP tool'u sadece execution. Sage'in tezi: anlama + eylem birleşimi. Phase 2 olmadan iddia, Phase 2 ile kanıt.
