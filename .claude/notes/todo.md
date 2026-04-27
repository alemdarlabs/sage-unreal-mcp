# Milestone 1.1 — Server Scaffolding ✓

> Started: 2026-04-27. Reference: `.claude/docs/mvp-roadmap.md` §1.1, ADR-012.
> **Status: COMPLETE — build green, 23/23 tests passing, smoke OK.**

## Plan

### A. Repository foundation
- [x] `.gitignore`
- [x] `.clang-format`
- [x] `.clang-tidy`
- [ ] `LICENSE` — **deferred** to ADR-014 (license selection)
- [x] `git init`

### B. Build system
- [x] `CMakeLists.txt` (root) — CMake 3.25+, modern targets, sanitizers/warnings interface libs
- [x] `vcpkg.json` (root) — manifest mode: `nlohmann-json`, `spdlog`, `cpp-httplib`, `catch2`
- [x] `CMakePresets.json` — `debug` (ASan+UBSan), `release`, `tsan` presets
- [x] HTTP library decision → ADR-013 (cpp-httplib)

### C. MCP protocol layer (`server/src/mcp/`)
- [x] `error_codes.h`
- [x] `types.h`
- [x] `tool.h`
- [x] `tool_registry.{h,cpp}`
- [x] `server.{h,cpp}`

### D. Transport (`server/src/transport/`)
- [x] `http_sse_server.{h,cpp}` (cpp-httplib bridge)

### E. Built-in tools (`server/src/tools/`)
- [x] `builtin.{h,cpp}` — `ping`

### F. Entry point
- [x] `server/src/main.cpp`

### G. Tests
- [x] `tests/CMakeLists.txt`
- [x] `tests/unit/test_jsonrpc.cpp` — 10 test
- [x] `tests/unit/test_tool_registry.cpp` — 6 test
- [x] `tests/unit/test_server.cpp` — 7 test

### H. Verification ✓
- [x] vcpkg manifest auto-install — 8 paket, 30 s (catch2, cpp-httplib, fmt, nlohmann-json, spdlog, brotli, vcpkg-cmake, vcpkg-cmake-config)
- [x] `cmake --preset debug` — Configuring done in 33.2 s, AppleClang 21.0.0
- [x] `cmake --build --preset debug` — 13/13 clean (transitive link refactor: spdlog PUBLIC sage-mcp, sage-server slim)
- [x] `ctest --preset debug` — **23/23 PASSED** under ASan + UBSan, 0.51 s total
- [x] Smoke `GET /healthz` → `{"status":"ok"}`
- [x] Smoke `POST /mcp initialize` → protocolVersion 2025-03-26 + tools capability + serverInfo
- [x] Smoke `POST /mcp tools/list` → ping descriptor with JSON Schema
- [x] Smoke `POST /mcp tools/call ping echo=hello` → `{echo:"hello", pong:true}` + structuredContent
- [x] Smoke notification → HTTP 202, log `Client signaled initialized`
- [x] Smoke unknown method → JSON-RPC -32601
- [x] Smoke SIGTERM → graceful shutdown

### I. Commit
- [x] `git init`
- [x] Initial commit (now)

## Toolchain (yapılan kurulum)

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
brew install llvm   # clang-format + clang-tidy (henüz invoke edilmedi)
```

Build:
```bash
VCPKG_ROOT=$HOME/vcpkg cmake --preset debug
VCPKG_ROOT=$HOME/vcpkg cmake --build --preset debug
VCPKG_ROOT=$HOME/vcpkg ctest --preset debug
```

## Açık Sorular

- ~~HTTP server karar~~ → ADR-013 (cpp-httplib)
- ~~HTTP+SSE tek-port vs çift-endpoint~~ → tek-port (Streamable HTTP), single-shot JSON; chunked SSE Milestone 1.6+
- ~~Capability handshake~~ → Phase 1 sadece `tools.listChanged=false`
- License → ADR-014 (Apache-2.0 / MIT / dual)
- clang-format / clang-tidy CI gate → DevOps Engineer scope; Milestone sonrası

## Review

### Yapıldı
- **23 source dosya** (~1153 LOC source) + 6 build/config + 1 ADR + 1 doc edit
  - Root: `.gitignore`, `.clang-format`, `.clang-tidy`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`
  - Server: `server/CMakeLists.txt` + 8 source (mcp/{error_codes.h, types.h, tool.h, tool_registry.{h,cpp}, server.{h,cpp}}, transport/http_sse_server.{h,cpp}, tools/builtin.{h,cpp}, main.cpp)
  - Tests: `tests/CMakeLists.txt` + 3 unit suites
  - Decisions: ADR-013 (HTTP server library)
  - Docs: tech-stack.md HTTP server satırı güncellendi
- Library decomposition (`sage-mcp`, `sage-transport`, `sage-tools`, `sage-server`, `sage-tests`); spdlog PUBLIC sage-mcp ile transitive doğru, duplicate-link warning sıfır.
- Heterogeneous lookup `unordered_map`'te → string_view dispatch zero-allocation.
- Tool exception trapping → handler throw'ları `InternalError` JSON-RPC response'una çevrilir, ASan altında kontrol edildi.
- ASan + UBSan + `-Werror` ile build, sanitizer-clean.

### Doğrulandı
- vcpkg manifest auto-install (8 paket) — 30 s
- cmake configure — 33.2 s
- cmake build — 13/13 target clean
- ctest — 23/23 PASSED
- sage-server smoke (curl) — 7/7 senaryo OK (initialize, tools/list, tools/call ping, ping rpc, notification, unknown method, SIGTERM)

### Sonraki Adım
1. Initial commit ✓ (now)
2. **Milestone 1.2 — Plugin Scaffolding** (`/unreal-architect` persona)
   - UPlugin yapısı (`SageBridge.uplugin`, `Source/SageBridge/`)
   - FWebSocketsModule client + reconnect (exponential backoff)
   - Heartbeat 15 s + handshake (slot_id formula per ADR-003)
   - Multi-editor labels (CLI argument + per-instance config)
   - BuildPlugin pipeline
