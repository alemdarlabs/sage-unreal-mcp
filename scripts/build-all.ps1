# Build sage-server + SageBridge plugin in one go.
# Usage:  .\scripts\build-all.ps1                 # debug preset
#         .\scripts\build-all.ps1 release
#         .\scripts\build-all.ps1 -SkipPlugin     # only the server
#         .\scripts\build-all.ps1 -SkipServer     # only the plugin

param(
    [Parameter(Position = 0)]
    [ValidateSet('debug', 'release', 'tsan')]
    [string]$Preset = 'debug',

    [switch]$SkipServer,
    [switch]$SkipPlugin,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot

if (-not $SkipServer) {
    Write-Host "`n=== [1/2] sage-server ($Preset) ===" -ForegroundColor Cyan
    & (Join-Path $ScriptDir 'build-server.ps1') $Preset -Clean:$Clean
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipPlugin) {
    Write-Host "`n=== [2/2] SageBridge plugin ===" -ForegroundColor Cyan
    & (Join-Path $ScriptDir 'build-plugin.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "`n==> All builds done." -ForegroundColor Green
