# Build SageBridge UE plugin standalone via UAT BuildPlugin.
# Output: build\plugin\

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Plugin = Join-Path $ProjectRoot 'plugin\SageBridge.uplugin'
$Output = Join-Path $ProjectRoot 'build\plugin'

$UeRoot = $env:SAGE_UE_ROOT
if (-not $UeRoot) {
    $UeRoot = 'C:\Program Files\Epic Games\UE_5.7'
}
$RunUAT = Join-Path $UeRoot 'Engine\Build\BatchFiles\RunUAT.bat'

if (-not (Test-Path $RunUAT)) {
    Write-Error "RunUAT not found: $RunUAT. Set SAGE_UE_ROOT to your UE 5.7 install."
    exit 1
}
if (-not (Test-Path $Plugin)) {
    Write-Error "Plugin not found: $Plugin"
    exit 1
}

$Platform = $env:SAGE_BUILD_PLATFORM
if (-not $Platform) { $Platform = 'Win64' }

New-Item -ItemType Directory -Force -Path $Output | Out-Null

Write-Host "==> Building plugin"
Write-Host "    Plugin:   $Plugin"
Write-Host "    Output:   $Output"
Write-Host "    Engine:   $UeRoot"
Write-Host "    Platform: $Platform"

& $RunUAT BuildPlugin `
    -Plugin="$Plugin" `
    -Package="$Output" `
    -Rocket `
    -TargetPlatforms="$Platform"

if ($LASTEXITCODE -ne 0) {
    Write-Error "BuildPlugin failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "==> Done. Packaged plugin under $Output"
