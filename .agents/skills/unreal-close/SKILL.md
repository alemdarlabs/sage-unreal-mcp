---
name: unreal-close
description: Close Unreal Editor safely on Windows first, with macOS fallback. Invoke only when the user explicitly asks to close/restart Unreal or an approved plugin binary swap requires the editor to exit.
---

# Close Unreal Editor

## Overview

Closes Unreal Editor processes for rebuild, plugin deploy, or restart workflows. This skill is Windows-first for current Sage work.

Closing a production project can lose unsaved work. Save through Unreal/MCP first when possible, and do not force-close unless the user explicitly approved it.

## Windows Recipe

Use PowerShell. Target a project path when more than one editor can be running.

```powershell
param(
    [string]$Project = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$processRows = Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe'"
if ($Project) {
    $needle = [System.IO.Path]::GetFullPath($Project)
    $processRows = $processRows | Where-Object {
        $_.CommandLine -and $_.CommandLine.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    }
}

if (-not $processRows) {
    Write-Output "no matching UnrealEditor.exe process"
    exit 0
}

$ids = @($processRows | ForEach-Object { [int]$_.ProcessId })
Write-Output ("closing UnrealEditor process id(s): " + ($ids -join ", "))

foreach ($id in $ids) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) {
        [void]$p.CloseMainWindow()
    }
}

$deadline = (Get-Date).AddSeconds(20)
do {
    Start-Sleep -Seconds 1
    $remaining = @($ids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if (-not $remaining) {
        Write-Output "UnrealEditor closed"
        exit 0
    }
} while ((Get-Date) -lt $deadline)

if (-not $Force) {
    Write-Output "UnrealEditor still running after graceful close request; rerun with -Force only after user approval"
    exit 1
}

foreach ($id in $remaining) {
    Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
}
Write-Output "forced UnrealEditor shutdown"
```

Example:

```powershell
.\close-unreal.ps1 -Project "<absolute-path-to-your-project.uproject>"
```

Force close, only after explicit user approval:

```powershell
.\close-unreal.ps1 -Project "<absolute-path-to-your-project.uproject>" -Force
```

## macOS Fallback

Use only on macOS hosts:

```bash
PROJECT="$1"

if [ -n "$PROJECT" ]; then
    pkill -TERM -f "$PROJECT" || { echo "no matching UnrealEditor running"; exit 0; }
else
    pkill -TERM -f UnrealEditor || { echo "no UnrealEditor running"; exit 0; }
fi

for i in 1 2 3 4 5 6 7 8; do
    sleep 1
    if ! pgrep -f UnrealEditor >/dev/null; then
        echo "UnrealEditor closed in ${i}s"
        exit 0
    fi
done

echo "still running; do not SIGKILL without explicit user approval"
exit 1
```

## Constraints

- Do not close a production editor with unsaved work unless the user approved that risk.
- Prefer `save_assets` / `save_level` before closing when a live editor and MCP server are available.
- On Windows, `CloseMainWindow()` is the graceful path; `Stop-Process -Force` is destructive and requires explicit approval.
- In multi-editor sessions, filter by `.uproject` path rather than closing every UnrealEditor process.
