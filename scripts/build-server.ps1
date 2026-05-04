# Build sage-server with vcvars-loaded MSVC env.
# Usage:  .\scripts\build-server.ps1                # debug preset (default)
#         .\scripts\build-server.ps1 release        # release preset
#         .\scripts\build-server.ps1 debug -Target sage-tests
#
# Loads VS 2022 vcvars64 via dev-shell.ps1, then runs cmake --build.
# This is the missing wrapper that prevents the "ninja idle, cl never spawns"
# trap when you run cmake from a plain PowerShell session.

param(
    [Parameter(Position = 0)]
    [ValidateSet('debug', 'release', 'tsan')]
    [string]$Preset = 'debug',

    [string]$Target = 'sage-server',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'dev-shell.ps1')

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

if ($Clean) {
    Write-Host "==> Clean: removing build\$Preset"
    Remove-Item -Recurse -Force "build\$Preset" -ErrorAction SilentlyContinue
}

Write-Host "==> cmake --build --preset $Preset --target $Target"
cmake --build --preset $Preset --target $Target
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$BinDir = Join-Path $ProjectRoot "build\$Preset\bin"
Write-Host "==> Done. Output: $BinDir"
Get-ChildItem $BinDir -Filter '*.exe' -ErrorAction SilentlyContinue |
    Select-Object Name, Length, LastWriteTime |
    Format-Table -AutoSize
