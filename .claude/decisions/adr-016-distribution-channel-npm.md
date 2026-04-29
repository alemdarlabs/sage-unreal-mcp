# ADR-016: Distribution Channel — npm-first

**Tarih:** 2026-04-29
**Durum:** Kabul Edildi

## Bağlam

Sage'in üç dağıtılacak parçası var:

1. **sage-server** — C++23 native binary (cross-platform)
2. **SageBridge plugin** — UE plugin (per-engine binary: 5.4, 5.5, 5.6, 5.7)
3. **MCP client config** — claude code / Cursor / Cline `mcp.json` entry (tek satır)

Aday kanallar:
- **Homebrew / apt / winget** — sistem paket yöneticileri (platform-spesifik)
- **GitHub Releases** — tarball + manuel install
- **Docker** — headless / CI senaryoları
- **npm** — AI dev tool ekosisteminin reflexi
- **FAB / UE Marketplace** — UE plugin için Epic'in resmi kanalı

**Hedef kitle gözlemi**: Sage'in birincil kullanıcısı *AI agent + UE geliştiricisi*. Bu kitle zaten Claude Code'u (`npm install -g @anthropic-ai/claude-code`) ve Codex CLI'yi (`npm install -g @openai/codex`) npm üzerinden yüklüyor. AI dev araç ekosisteminin "tek satır install" reflexi npm.

## Karar

**Birincil dağıtım kanalı**: `@alemdarlabs/sage-mcp` npm package.

```bash
npm install -g @alemdarlabs/sage-mcp
sage init    # MCP client config + UE plugin install wizard
```

### Mekanizma

1. **npm package**: `package.json` `bin` field → `sage` komutu PATH'e eklenir.
2. **postinstall script**: platform tespit eder (darwin-arm64, darwin-x64, linux-x64, linux-arm64, win32-x64) ve GitHub Releases'tan platforma uygun native binary indirir → `~/.sage-mcp/bin/sage-server` altına kurar.
3. **`sage` CLI wrapper** (Node.js): native binary'yi exec eder. Node.js gerekmez — sadece npm install pathway için kullanılır.
4. **`sage init`**: kullanıcıyı 3 adımda gezdirir:
   - Hangi UE projesi? → uproject path al
   - Hangi MCP client? → `claude mcp add` veya Cursor/Cline `mcp.json` patch
   - Engine version tespit → uygun `SageBridge-5.X.zip` indir → `Plugins/SageBridge/` altına aç

### İkincil Kanallar (paralel)

- **GitHub Releases**: airgapped / kurum proxy / manuel kurulum için doğrudan platform tarball'ları
- **Homebrew tap** (`alemdarlabs/homebrew-sage`): macOS Unix-native power-user deneyimi
- **FAB**: UE plugin için ileri faz (community büyüdüğünde, Epic onay süresi tolere edilebilir hale geldiğinde)

## Gerekçe

1. **Hedef kitle reflexi**: Claude Code ve Codex CLI npm üzerinden geliyor → AI agent geliştirici tabanı `npm i -g` komutuna alışkın. Sage'in aynı kanaldan gelmesi onboarding friction'ı sıfıra indirir.
2. **Cross-platform tek komut**: npm package darwin/linux/win her üçünde aynı `npm i -g` komutuyla yüklenir. Homebrew (Mac-only) + apt (Linux-only) + winget (Win-only) parçalı deneyim sunar.
3. **Sürüm yönetimi bedava**: npm semver, `npm outdated`, `npm update -g`, `npm uninstall` zaten kullanıcının bildiği akış. CI'de tek `npm publish` adımı yeter.
4. **Discoverability**: `npmjs.com` araması + GitHub package ekosistemi + AI dev tool listings ("awesome-mcp-servers" vb.) npm package referans verir; npm registry organic discovery sunar.
5. **Wrapper modeli ispatlandı**: Codex CLI 2024'te Rust binary'ye geçti ama npm wrapper'ı korudu — postinstall script ile binary indirme pattern'i AI tooling ekosisteminde standart.
6. **ADR-013 (cpp-httplib) + ADR-015 (ixwebsocket) ile tutarlı disiplin**: "Hedef kullanım profiline göre right-size" — hedef kitle npm reflexi ile geliyor, kanalı ona göre seç.

## Reddedilen Alternatifler

- **Homebrew-first**: macOS-only; Linux/Windows kullanıcılarına ikinci sınıf deneyim. Bottle CI Mac runner'larıyla sınırlı. Discoverability düşük (`brew search` AI tool kitlesinde reflex değil).
- **GitHub Releases-only**: Manuel PATH yapılandırması, manuel update. Power user OK; mainstream onboarding'i düşürür. Update mekanizması yok (kullanıcı yeni release'i fark etmek zorunda).
- **Docker-first**: UE Editor ↔ plugin local IPC gerektiriyor; container içinde sage-server izole edilirse host UE plugin'le bridge handshake'i karmaşıklaşır. Headless CI senaryosu için iyi ama mainstream değil.
- **FAB-only**: UE plugin için resmi kanal ama sage-server'ı kapsamaz; iki ayrı kanal (Marketplace + manuel server install) kullanıcıyı kafa karıştırır. FAB onay süresi (aylar) erken adoption'ı bloklar.
- **pip / PyPI**: Python ekosistemi MCP server'larında (FastMCP vb.) yaygın ama Sage'in hedef kitlesi (Claude Code/Codex kullanıcıları) zaten npm reflexinde.

## Plugin Dağıtımı — Tamamlayıcı Notlar

UE plugin npm package'a **embedded değil**:
- Boyut: per-engine binary 30-50MB, 4 engine version × 3 platform = 12 ZIP
- Versiyon koplmaması: server semver bağımsız, plugin engine version'a bağlı
- Çoğaltma maliyeti: kullanıcı tek engine kullanıyor, gereksiz indirme

Bunun yerine: `sage init` veya `sage update --plugin <project>` GitHub Releases'tan ilgili `SageBridge-<engine>-<platform>.zip`'i çeker, hedef projenin `Plugins/SageBridge/` altına açar, uproject `Plugins` listesine ekler.

## Sonuçlar

**Olumlu:**
- Tek install komutu (`npm i -g @alemdarlabs/sage-mcp`) cross-platform
- AI agent dev kitlesinin reflex kanalı → onboarding friction minimum
- npm semver / update / discoverability altyapısı bedavaya gelir
- CI tek pipeline: GitHub Actions matrix → multi-platform binary build → GitHub Release upload + `npm publish` paralel
- Power user'lar için ikincil kanallar (Homebrew, GitHub Releases) paralel mevcut

**Olumsuz:**
- **Postinstall network dependency**: 50-100MB binary download fail ederse `npm i -g` fail olur. Kurum proxy / npm registry mirror senaryolarında `SAGE_BINARY_URL` veya `SAGE_BINARY_PATH` env var ile manuel fallback gerekir. Postinstall script bu env var'ları okuyacak şekilde yazılmalı.
- **npm CLI bağımlılığı**: Pure C++ / GameDev kullanıcısında Node.js + npm yüklü olmayabilir. Bu kullanıcılar için ikincil Homebrew/GitHub kanalları paralel tutulur.
- **Codesigning maliyeti**: macOS notarization (Apple Developer ID, $99/yıl) + Windows EV code signing cert ($300-500/yıl) zorunlu — yoksa Gatekeeper/SmartScreen kullanıcıyı düşürür. CI'de sign step ve secrets gerekir.
- **Per-engine plugin matrisi**: GitHub Actions matrix [5.4, 5.5, 5.6, 5.7] × [Win64, Mac, Linux] = 12 build/release. Build süresi ~30-60dk, CI maliyet kalemi.

## Etkilenen Belgeler ve Sonraki Adımlar

| Adım | Hedef | Faz |
|---|---|---|
| `package.json` skeleton + `bin` field (`sage` komutu) | Yeni dosya repo root'ta | Phase 5 başlangıç |
| `scripts/postinstall.js` — platform tespit + binary download | Yeni dosya | Phase 5 başlangıç |
| `sage` CLI wrapper (Node.js, exec native binary) | `bin/sage.js` | Phase 5 başlangıç |
| `sage init` komutu — server'a sub-command olarak | `server/src/cli/init.cpp` | Phase 5 |
| `.github/workflows/release.yml` — multi-platform CI matrix | Yeni workflow | Phase 5 |
| Codesigning pipeline (Apple Developer ID, Windows EV cert) | CI secrets + sign step | Phase 5 (cert tedarik sonrası) |
| `homebrew-sage` tap repo (paralel kanal) | Yeni repo | Phase 5 ikinci dalga |
| `docs/getting-started.md` — npm install + `sage init` onboarding | Yeni doküman | Phase 5 |
| FAB plugin başvurusu | Plugin paketleme + Epic submission | Mainstream faz |

## Versioning Notu

- npm package version === sage-server version (semver)
- Plugin version: `1.0.0+ue5.7` formatında (server major === plugin major; engine version build metadata)
- Protocol version: server↔plugin handshake'te negotiate; backwards-compat 1 major version
- Mismatch'te server reddetsin: `"Plugin v0.9 incompatible with server v1.2 — run `sage update --plugin`"`
