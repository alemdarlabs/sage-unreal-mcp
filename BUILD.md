# Build Guide

Sage Unreal MCP — build instructions for macOS, Linux, and Windows.

The codebase is intentionally cross-platform: server is C++23 + CMake + vcpkg manifest mode; plugin is Unreal C++ built with UnrealBuildTool. All commands below produce identical artifacts on every supported host.

## Toolchain

| Tool | Purpose | macOS / Linux | Windows |
|---|---|---|---|
| Git | SCM | preinstalled / `brew install git` | `winget install Git.Git` |
| CMake ≥ 3.25 | build system | `brew install cmake` | `choco install cmake` |
| Ninja | generator | `brew install ninja` | `choco install ninja` |
| vcpkg | C++ deps | clone + bootstrap (below) | same |
| LLVM | clang-format / clang-tidy | `brew install llvm` | `choco install llvm` |
| Unreal Engine 5.7 | plugin build | Epic Games Launcher | Epic Games Launcher |

## Environment Variables

| Var | Purpose | macOS default | Windows default |
|---|---|---|---|
| `VCPKG_ROOT` | vcpkg manifest install root | `$HOME/vcpkg` | `%USERPROFILE%\vcpkg` |
| `SAGE_UE_ROOT` | Unreal install path used by BuildPlugin | `<absolute-path-to-Unreal-Engine-install>` | `<absolute-path-to-Unreal-Engine-install>` |
| `SAGE_HTTP_PORT` (opt) | server HTTP port | 7777 | 7777 |
| `SAGE_LOG_LEVEL` (opt) | spdlog level | `info` | `info` |

### macOS / Linux

```bash
cat >> ~/.zshrc <<'EOF'
export VCPKG_ROOT="$HOME/vcpkg"
export SAGE_UE_ROOT="<absolute-path-to-Unreal-Engine-install>"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
EOF
source ~/.zshrc
```

### Windows (PowerShell, persistent)

```powershell
[Environment]::SetEnvironmentVariable("VCPKG_ROOT",   "$env:USERPROFILE\vcpkg", "User")
[Environment]::SetEnvironmentVariable("SAGE_UE_ROOT", "<absolute-path-to-Unreal-Engine-install>", "User")
```

Re-open the shell so the variables take effect.

## First-time Setup

### 1. vcpkg

```bash
# macOS / Linux
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
```

```powershell
# Windows
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\vcpkg"
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
```

### 2. Server: configure → build → test

The CMake presets are platform-agnostic. Same commands on every host:

```bash
cmake --preset debug          # Debug + ASan/UBSan; vcpkg installs deps on first run
cmake --build --preset debug
ctest --preset debug
```

Available presets:
- `debug` — Debug + ASan + UBSan (default)
- `release` — `-O3`, no sanitizers
- `tsan` — Debug + ThreadSanitizer (macOS/Linux only; mutually exclusive with ASan)

Server binary: `build/debug/bin/sage-server`. Tests: `build/debug/bin/sage-tests`.

### 3. Plugin: BuildPlugin via UnrealBuildTool

```bash
# macOS / Linux
scripts/build-plugin.sh
```

```powershell
# Windows
scripts\build-plugin.ps1
```

Both wrappers invoke:
```
RunUAT BuildPlugin -Plugin=$PWD/plugin/SageBridge.uplugin -Package=$PWD/build/plugin
```

Output: standalone packaged plugin under `build/plugin/`. Drop into any UE 5.7 project's `Plugins/` directory to consume.

## Smoke Test

```bash
./build/debug/bin/sage-server &
sleep 1
curl -s http://127.0.0.1:7777/healthz                                   # → {"status":"ok"}
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"initialize","id":1,"params":{}}' \
  http://127.0.0.1:7777/mcp                                             # → initialize result
kill %1
```

## Cross-platform Notes

- **Line endings**: enforced by `.gitattributes`. Source files (`.cpp`, `.h`, `.cs`, `.cmake`, JSON, Markdown) are LF. Windows scripts (`.bat`, `.ps1`) are CRLF.
- **vcpkg cache**: per-platform. `vcpkg_installed/` is gitignored. First `cmake --preset debug` on a new host rebuilds deps (~30 s–2 min).
- **Sanitizers**: ASan + UBSan on Clang/GCC. MSVC supports ASan but **not UBSan or TSan**. The `debug` preset works on all hosts; `tsan` is macOS/Linux only.
- **C++23**: requires AppleClang ≥ 17 (Xcode 16+), MSVC ≥ 19.40 (VS 17.10+), or GCC ≥ 14. Verify with `clang --version` / `cl.exe`.
- **Signal handling**: server installs `signal(SIGINT)` + `signal(SIGTERM)`. macOS/Linux full support; Windows handles `Ctrl+C` via SIGINT but SIGTERM is limited. A Phase 2 polish can switch to `SetConsoleCtrlHandler` on Windows if needed.
- **UE plugin path**: kept abstract via `SAGE_UE_ROOT`. `scripts/build-plugin.{sh,ps1}` resolves the platform-correct UAT entry point.

## Troubleshooting

- **`find_package(httplib) failed`**: `VCPKG_ROOT` is unset, or you ran `cmake -B build` instead of `cmake --preset debug`. Presets wire the toolchain file automatically.
- **`'std::expected' is not a member of 'std'`**: compiler too old. Upgrade to AppleClang 17 / MSVC 19.40 / GCC 14.
- **Plugin compile fails with "engine not found"**: `SAGE_UE_ROOT` wrong or UE 5.7 not installed. Check `ls "$SAGE_UE_ROOT/Engine/Build/BatchFiles"`.
- **Linker warning "duplicate libraries"**: this was fixed by transitive-link refactor in commit `3641985`; if you see it now, you're on an older revision.
