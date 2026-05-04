param(
    [string] $Configuration = "debug",
    [string] $HostAddress = "127.0.0.1",
    [int] $Port = 7777,
    [int] $BridgePort = 7778
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ServerExe = Join-Path $RepoRoot "build\$Configuration\bin\sage-server.exe"

if (-not (Test-Path -LiteralPath $ServerExe)) {
    Write-Error "sage-server.exe not found: $ServerExe. Build first with: . .\scripts\dev-shell.ps1; cmake --build --preset $Configuration --target sage-server"
}

Write-Host "Starting Sage server:"
Write-Host "  HTTP MCP: http://$HostAddress`:$Port"
Write-Host "  Bridge:   ws://$HostAddress`:$BridgePort/bridge"
Write-Host "  Binary:   $ServerExe"

$ServerArgs = @(
    "--http",
    "--host", $HostAddress,
    "--port", $Port,
    "--bridge-port", $BridgePort
)

& $ServerExe @ServerArgs
