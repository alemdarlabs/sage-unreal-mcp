---
name: unreal-open
description: Launch Unreal Editor for an explicit .uproject path on Windows first, with macOS fallback. Invoke only when the user asks to open/reopen Unreal or after an approved plugin rebuild/deploy flow.
---

# Open Unreal Editor

## Overview

Launches Unreal Editor for a specific `.uproject`. This skill is Windows-first for the current Sage production workflow and keeps macOS instructions as a fallback only.

Always prefer an explicit project path. Do not assume a sample project or any Mac-only path.

## Windows Recipe

Use PowerShell. Starting Unreal is an interactive action, so a visible editor window is expected.

```powershell
param(
    [Parameter(Mandatory = $true)]
    [string]$Project,

    [string]$UnrealEditor = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Project)) {
    throw "Project not found: $Project"
}

if (-not $UnrealEditor) {
    $ueRoot = $env:SAGE_UE_ROOT
    if (-not $ueRoot) {
        $ueRoot = "C:\Program Files\Epic Games\UE_5.7"
    }

    $candidate = Join-Path $ueRoot "Engine\Binaries\Win64\UnrealEditor.exe"
    if (Test-Path -LiteralPath $candidate) {
        $UnrealEditor = $candidate
    }
}

if ($UnrealEditor -and (Test-Path -LiteralPath $UnrealEditor)) {
    Start-Process -FilePath $UnrealEditor -ArgumentList @("`"$Project`"") -WindowStyle Normal
    Write-Output "launched UnrealEditor.exe: $Project"
} else {
    Start-Process -FilePath $Project -WindowStyle Normal
    Write-Output "launched via .uproject association: $Project"
}
```

Example:

```powershell
.\open-unreal.ps1 -Project "D:\GameDev\Kale\Kale.uproject" -UnrealEditor "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
```

## macOS Fallback

Use only on macOS hosts:

```bash
PROJECT="$1"
if [ ! -f "$PROJECT" ]; then
    echo "ERROR: project not found: $PROJECT" >&2
    exit 1
fi

open "$PROJECT"
echo "launched: $PROJECT"
```

## Readiness Check

Do not infer readiness from process launch alone. Verify the bridge connection through the MCP server when it is running:

```powershell
$body = @{
    jsonrpc = "2.0"
    method = "tools/call"
    id = 1
    params = @{
        name = "list_editors"
        arguments = @{}
    }
} | ConvertTo-Json -Depth 8

Invoke-RestMethod -Uri "http://127.0.0.1:7777/mcp" -Method Post -ContentType "application/json" -Body $body
```

## Constraints

- Do not start Unreal if the user said not to start the editor or server.
- In multi-project sessions, always pass the intended `.uproject` path explicitly.
- Opening Unreal proves only process launch, not plugin health. Use `list_editors` / `wait_for_editor` for connection proof.
- Pair with `unreal-close` only after explicit user approval for closing an editor.
