# Download a KuzuDB prebuilt release into third_party/kuzu/.
# Windows companion to scripts/fetch-kuzu.sh.
#
# vcpkg has no port for kuzu (community lib). FetchContent could build from
# source but takes 10+ minutes; the upstream prebuilt archive ships in
# seconds. The CMake build picks it up via third_party/kuzu/.
#
# Override version with KUZU_VERSION env var.

$ErrorActionPreference = 'Stop'

$Version = if ($env:KUZU_VERSION) { $env:KUZU_VERSION } else { 'v0.11.3' }
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$TargetDir = if ($args.Count -gt 0) { $args[0] } else { Join-Path $ProjectRoot 'third_party\kuzu' }

# Detect arch — Windows prebuilds: libkuzu-windows-x86_64.zip (only x64 published).
$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') {
    Write-Error "unsupported Windows arch: $arch (kuzu publishes only x86_64 on Windows)"
    exit 1
}
$Asset = 'libkuzu-windows-x86_64.zip'
$Url = "https://github.com/kuzudb/kuzu/releases/download/$Version/$Asset"

if ((Test-Path $TargetDir) -and (Get-ChildItem -Path $TargetDir -Filter 'kuzu.hpp' -Recurse -ErrorAction SilentlyContinue)) {
    Write-Host "kuzu already present: $TargetDir"
    Get-ChildItem -Path $TargetDir -Directory | ForEach-Object { Write-Host "  $($_.FullName)" }
    exit 0
}

Write-Host "==> downloading $Url"
New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
$Tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ("kuzu-fetch-" + [System.Guid]::NewGuid().ToString('N'))) -Force
$ZipPath = Join-Path $Tmp 'kuzu.zip'
try {
    # TLS 1.2 explicit — older PowerShell defaults can fail GitHub
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
    Expand-Archive -Path $ZipPath -DestinationPath $TargetDir -Force
} finally {
    Remove-Item -Recurse -Force -Path $Tmp -ErrorAction SilentlyContinue
}

Write-Host "==> extracted into $TargetDir"
Get-ChildItem -Path $TargetDir -Recurse -Include '*.h','*.hpp','*.dll','*.lib' -ErrorAction SilentlyContinue |
    Select-Object -First 20 |
    ForEach-Object { Write-Host "  $($_.FullName)" }
