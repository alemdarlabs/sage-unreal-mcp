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

The project is in **architecture design phase** — no code written yet. Design decisions are tracked in [`.claude/decisions/`](.claude/decisions/), system documentation in [`.claude/docs/`](.claude/docs/).

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

## Build & Run Commands

```bash
# Henüz implement edilmedi. Tasarım fazında.

# Server build (planlanan):
# cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
# cmake --build build --config Release

# Plugin: Unreal Editor üzerinden veya UnrealBuildTool plugin compile

# Server start (planlanan):
# ./build/sage-server --config sage.toml
```

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
- [Decisions (ADR)](.claude/decisions/) — Architectural Decision Records (12 ADRs)
