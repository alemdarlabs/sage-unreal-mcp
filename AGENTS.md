# Sage Unreal MCP - Agent Guide

Bu dosya repo içindeki canonical çalışma rehberidir. Eski hidden workspace yapısı kaldırıldı; kalıcı dokümanlar `docs/`, aktif skill tanımları `.agents/skills/` altındadır.

## İletişim

- Kullanıcıyla Türkçe konuş. Kod identifier'ları, komutlar, path'ler ve commit mesajları İngilizce kalabilir.
- Kullanıcı özellikle "önce onay al" diyorsa dosya değiştirme, sadece plan ve gerekçe ver.
- Kullanıcı "derleme yapma", "server başlatma" veya benzeri sınır koyarsa bu sınır kapalıdır; tekrar açılana kadar build/server/PIE başlatma.
- Yapılan işin doğrulamasını ayrı söyle: dosya düzenleme, build, test, deploy, server health ve runtime proof aynı şey değildir.

## Uzmanlık Yönlendirme

İşin baskın alanına göre mevcut `.agents/skills/` skill'lerinden en uygun olanı kullan. Eksik özel skill varsa kaynak kodu okuyarak aynı uzmanlık disipliniyle ilerle.

| Alan | Öncelikli skill / yaklaşım |
|---|---|
| Unreal C++ plugin, UCLASS, AssetRegistry, Slate, Live Coding | Source-backed Unreal inceleme; review için `code-review` |
| C++23 server, CMake, vcpkg, paketleme | `devops-assistant`, gerekirse `code-review` |
| MCP protocol, JSON-RPC, tool schema tasarımı | Kaynak + `knowledge-structuring` |
| Tool parity, rakip repo kıyasları | `competitive-intelligence`, `deep-research-synthesizer` |
| Docs, ADR, bilgi mimarisi | `knowledge-structuring`, `tone-style-enforcer`, `scqa-writing` |
| CI/CD, release, npm dağıtımı | `devops-assistant` |
| Güvenlik, auth, sandbox | Source-backed security review |
| Test stratejisi ve regression riski | `code-review`, QA lens |

## Ürün Durumu

Sage Unreal MCP, Unreal Engine için C++23 tabanlı bir MCP server ve `SageBridge` Unreal plugin'inden oluşur. Sistem artık KuzuDB tabanlı graph runtime kullanmaz; ADR-018 ile bu katman kaldırıldı. Project understanding tarafı live Unreal inspection, reflection, AssetRegistry, source search ve domain-specific diagnostics üzerinden ilerler.

Güncel audit komutu:

```powershell
.\scripts\audit-tools.ps1 -Json
```

2026-06-04 kaynak audit sonucu:

- `server_tool_count`: 1243
- `plugin_handler_count`: 1191
- `plugin_without_schema`: 0
- `schema_stub_count`: 0
- `plugin_not_implemented_count`: 0
- `schema_without_plugin`: sadece server-only `jobs.*` yönetim tool'ları

Repo public görünebilir, fakat bu açık kaynak lisansı anlamına gelmez. LICENSE dosyası yoksa kod için permissive kullanım hakkı varsayma. Dağıtım stratejisi npm-first binary paketleme + ileride runtime auth/license gate şeklindedir.

## Canonical Klasör Yapısı

```text
sage-unreal-mcp/
|-- .agents/                 # Codex skill definitions
|-- .github/                 # CI / release workflows
|-- docs/
|   |-- adr/                 # Architectural Decision Records
|   |-- architecture/        # Active architecture docs
|   |-- engineering/         # API, build, project structure, pipelines
|   |-- release/             # npm / binary distribution notes
|   |-- research/            # Reference parity and market research
|   `-- archive/             # Historical notes, retired designs, gap logs
|-- npm/                     # npm package wrapper and postinstall logic
|-- plugin/                  # Unreal SageBridge plugin
|-- scripts/                 # Build, package, smoke, audit helpers
|-- server/                  # C++23 MCP server
|-- tests/                   # Catch2 unit/integration tests
|-- AGENTS.md                # This file
|-- README.md
|-- BUILD.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- package.json
`-- vcpkg.json
```

Kök dizine geçici log, smoke output, debugger dump veya tek seferlik analiz dosyası bırakma. Bunlar `artifacts/`, `logs/`, işletim sistemi temp dizini veya build çıktısı altında kalmalı ve git'e girmemelidir.

## Önemli Dokümanlar

- [Architecture](docs/architecture/architecture.md)
- [Tech Stack](docs/engineering/tech-stack.md)
- [API Spec](docs/engineering/api-spec.md)
- [Project Structure](docs/engineering/project-structure.md)
- [Compile Coordination](docs/engineering/compile-coordination.md)
- [Blueprint to C++ Pipeline](docs/engineering/bp-cpp-conversion-pipeline.md)
- [Transactions](docs/engineering/transactions.md)
- [ADR Index](docs/adr/)
- [npm Distribution Plan](docs/release/npm-distribution-plan.md)
- [Reference Tool Parity Research](docs/research/tool-parity/)

Historical gap logs and old working notes live under `docs/archive/working-notes/`. Bunları aktif doğruluk kaynağı gibi kullanma; önce source ve audit scriptleriyle doğrula.

## Build ve Run

Derleme komutları, kullanıcı açıkça istemediği sürece çalıştırılmaz.

```powershell
# Server
$env:VCPKG_ROOT = "$HOME\vcpkg"
cmake --build --preset debug --target sage-server

# Plugin package
.\scripts\build-plugin.ps1

# Tool audit, build gerektirmez
.\scripts\audit-tools.ps1 -Json
```

Server manuel debug için:

```powershell
$env:SAGE_REPO_ROOT = "D:\Steamworks\sage-unreal-mcp"
$env:SAGE_UE_ROOT = "C:\Program Files\Epic Games\UE_5.7"
$env:SAGE_LOG_LEVEL = "info"
.\build\debug\bin\sage-server.exe
```

npm release hattı:

```powershell
npm run package:assets
npm run release:check
```

GitHub Actions npm publish için `NPM_TOKEN` secret gerekir. Bu secret yoksa publish adımı fail-fast davranmalıdır. GitHub-hosted runner üzerinde Unreal Engine olmadığı için release workflow source `SageBridge` plugin asset paketler; `RunUAT BuildPlugin` sadece UE kurulu self-hosted Windows runner üzerinde yapılabilir.

## Production Project Guard

Gerçek Unreal projelerinde destructive op disiplini zorunludur:

- Major Blueprint mutation, reparent, delete veya conversion öncesi `bp.full_dump` al.
- `confirmed:true` isteyen tool'larda kullanıcıdan explicit onay almadan ilerleme.
- Production projede manuel `Remove-Item -Recurse`, plugin `Source/` wipe, `.uproject` Modules silme gibi işlemleri otomatik yapma.
- Multi-editor ortamında `_editor` parametresini explicit ver.
- Source control durumunu kontrol etmeden geniş mutation yapma.
- Kale/HeroFlight gibi hedeflere deploy istendiğinde build, copy, enable ve hash doğrulama adımlarını ayrı ayrı raporla.

## Engineering Rules

- Önce gerçek kaynak dosyaları oku; eski notlara veya hafızaya tek başına güvenme.
- Repo kirli olabilir. Kullanıcıya ait değişiklikleri revert etme.
- Manuel dosya editlerinde `apply_patch` kullan. Mekanik taşıma/silme için native PowerShell veya git komutları kullanılabilir.
- Windows'ta recursive delete/move öncesi path'in workspace içinde olduğunu doğrula.
- Unreal editor mutation kodu GameThread'e marshal edilmeli.
- MCP schema değişikliğinde `tools/list` ve `scripts/audit-tools.ps1` ile parity kontrolü yap.
- C++ plugin deploy için üçlü kural geçerli: `.uplugin`, `Binaries/<Platform>/`, `Source/`.
- Repo skill'leri Windows-first veya gerçek cross-platform olmalı; macOS-only tarifleri production workflow için canonical yazma.
- Build/test çalıştıysa sonucu söyle; çalışmadıysa "çalıştırmadım" de.

## Kapanış Kontrolü

Bir işi bitirmeden önce minimum kontrol:

```powershell
git status --short
rg -n "\.claude|\.Codex" README.md docs server plugin scripts npm --glob '!docs/archive/**' --glob '!build/**'
.\scripts\audit-tools.ps1 -Json
```

Derleme sadece kullanıcı doğru zamanı söylediğinde yapılır.
